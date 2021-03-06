/*
 * DriveMovement.cpp
 *
 *  Created on: 17 Jan 2015
 *      Author: David
 */

#include "DriveMovement.h"
#include "DDA.h"
#include "Move.h"
#include "RepRap.h"
#include "Libraries/Math/Isqrt.h"
#include "Kinematics/LinearDeltaKinematics.h"

// Static members

DriveMovement *DriveMovement::freeList = nullptr;
int DriveMovement::numFree = 0;
int DriveMovement::minFree = 0;

void DriveMovement::InitialAllocate(unsigned int num)
{
	while (num != 0)
	{
		freeList = new DriveMovement(freeList);
		++numFree;
		--num;
	}
	ResetMinFree();
}

DriveMovement *DriveMovement::Allocate(size_t drive, DMState st)
{
	DriveMovement * const dm = freeList;
	if (dm != nullptr)
	{
		freeList = dm->nextDM;
		--numFree;
		if (numFree < minFree)
		{
			minFree = numFree;
		}
		dm->nextDM = nullptr;
		dm->drive = (uint8_t)drive;
		dm->state = st;
	}
	return dm;
}

// Constructors
DriveMovement::DriveMovement(DriveMovement *next) : nextDM(next)
{
}

// Non static members

// Prepare this DM for a Cartesian axis move
void DriveMovement::PrepareCartesianAxis(const DDA& dda, const PrepParams& params)
{
	const float stepsPerMm = (float)totalSteps/dda.totalDistance;
	mp.cart.twoCsquaredTimesMmPerStepDivA = roundU64((double)(DDA::stepClockRateSquared * 2)/((double)stepsPerMm * (double)dda.acceleration));

	// Acceleration phase parameters
	mp.cart.accelStopStep = (uint32_t)(dda.accelDistance * stepsPerMm) + 1;
	mp.cart.compensationClocks = mp.cart.accelCompensationClocks = 0;

	// Constant speed phase parameters
	mp.cart.mmPerStepTimesCKdivtopSpeed = roundU32(((float)((uint64_t)DDA::stepClockRate * K1))/(stepsPerMm * dda.topSpeed));

	// Deceleration phase parameters
	// First check whether there is any deceleration at all, otherwise we may get strange results because of rounding errors
	if (dda.decelDistance * stepsPerMm < 0.5)
	{
		mp.cart.decelStartStep = totalSteps + 1;
		twoDistanceToStopTimesCsquaredDivA = 0;
	}
	else
	{
		mp.cart.decelStartStep = (uint32_t)(params.decelStartDistance * stepsPerMm) + 1;
		const uint64_t initialDecelSpeedTimesCdivASquared = isquare64(params.topSpeedTimesCdivA);
		twoDistanceToStopTimesCsquaredDivA = initialDecelSpeedTimesCdivASquared + roundU64((params.decelStartDistance * (DDA::stepClockRateSquared * 2))/dda.acceleration);
	}

	// No reverse phase
	reverseStartStep = totalSteps + 1;
	mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA = 0;
}

// Prepare this DM for a Delta axis move
void DriveMovement::PrepareDeltaAxis(const DDA& dda, const PrepParams& params)
{
	const float stepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive);
	const float A = params.initialX - params.dparams->GetTowerX(drive);
	const float B = params.initialY - params.dparams->GetTowerY(drive);
	const float aAplusbB = A * dda.directionVector[X_AXIS] + B * dda.directionVector[Y_AXIS];
	const float dSquaredMinusAsquaredMinusBsquared = params.diagonalSquared - fsquare(A) - fsquare(B);
	const float h0MinusZ0 = sqrtf(dSquaredMinusAsquaredMinusBsquared);
	mp.delta.hmz0sK = roundS32(h0MinusZ0 * stepsPerMm * DriveMovement::K2);
	mp.delta.minusAaPlusBbTimesKs = -roundS32(aAplusbB * stepsPerMm * DriveMovement::K2);
	mp.delta.dSquaredMinusAsquaredMinusBsquaredTimesKsquaredSsquared = roundS64(dSquaredMinusAsquaredMinusBsquared * fsquare(stepsPerMm * DriveMovement::K2));
	mp.delta.twoCsquaredTimesMmPerStepDivA = roundU64((double)(2 * DDA::stepClockRateSquared)/((double)stepsPerMm * (double)dda.acceleration));

	// Calculate the distance at which we need to reverse direction.
	if (params.a2plusb2 <= 0.0)
	{
		// Pure Z movement. We can't use the main calculation because it divides by a2plusb2.
		direction = (dda.directionVector[Z_AXIS] >= 0.0);
		reverseStartStep = totalSteps + 1;
	}
	else
	{
		// The distance to reversal is the solution to a quadratic equation. One root corresponds to the carriages being below the bed,
		// the other root corresponds to the carriages being above the bed.
		const float drev = ((dda.directionVector[Z_AXIS] * sqrtf(params.a2b2D2 - fsquare(A * dda.directionVector[Y_AXIS] - B * dda.directionVector[X_AXIS])))
							- aAplusbB)/params.a2plusb2;
		if (drev > 0.0 && drev < dda.totalDistance)		// if the reversal point is within range
		{
			// Calculate how many steps we need to move up before reversing
			const float hrev = dda.directionVector[Z_AXIS] * drev + sqrtf(dSquaredMinusAsquaredMinusBsquared - 2 * drev * aAplusbB - params.a2plusb2 * fsquare(drev));
			const int32_t numStepsUp = (int32_t)((hrev - h0MinusZ0) * stepsPerMm);

			// We may be almost at the peak height already, in which case we don't really have a reversal.
			if (numStepsUp < 1 || (direction && (uint32_t)numStepsUp <= totalSteps))
			{
				reverseStartStep = totalSteps + 1;
			}
			else
			{
				reverseStartStep = (uint32_t)numStepsUp + 1;

				// Correct the initial direction and the total number of steps
				if (direction)
				{
					// Net movement is up, so we will go up a bit and then down by a lesser amount
					totalSteps = (2 * numStepsUp) - totalSteps;
				}
				else
				{
					// Net movement is down, so we will go up first and then down by a greater amount
					direction = true;
					totalSteps = (2 * numStepsUp) + totalSteps;
				}
			}
		}
		else
		{
			reverseStartStep = totalSteps + 1;
		}
	}

	// Acceleration phase parameters
	mp.delta.accelStopDsK = roundU32(dda.accelDistance * stepsPerMm * K2);

	// Constant speed phase parameters
	mp.delta.mmPerStepTimesCKdivtopSpeed = roundU32(((float)DDA::stepClockRate * K1)/(stepsPerMm * dda.topSpeed));

	// Deceleration phase parameters
	// First check whether there is any deceleration at all, otherwise we may get strange results because of rounding errors
	if (dda.decelDistance * stepsPerMm < 0.5)
	{
		mp.delta.decelStartDsK = 0xFFFFFFFF;
		twoDistanceToStopTimesCsquaredDivA = 0;
	}
	else
	{
		mp.delta.decelStartDsK = roundU32(params.decelStartDistance * stepsPerMm * K2);
		twoDistanceToStopTimesCsquaredDivA = isquare64(params.topSpeedTimesCdivA) + roundU64((params.decelStartDistance * (DDA::stepClockRateSquared * 2))/dda.acceleration);
	}
}

// Prepare this DM for an extruder move
void DriveMovement::PrepareExtruder(const DDA& dda, const PrepParams& params, bool doCompensation)
{
	const float dv = dda.directionVector[drive];
	const float stepsPerMm = reprap.GetPlatform().DriveStepsPerUnit(drive) * fabsf(dv);
	mp.cart.twoCsquaredTimesMmPerStepDivA = roundU64((double)(DDA::stepClockRateSquared * 2)/((double)stepsPerMm * (double)dda.acceleration));

	// Calculate the pressure advance parameter
	const float compensationTime = (doCompensation && dv > 0.0) ? reprap.GetPlatform().GetPressureAdvance(drive - reprap.GetGCodes().GetTotalAxes()) : 0.0;
	mp.cart.compensationClocks = roundU32(compensationTime * (float)DDA::stepClockRate);
	mp.cart.accelCompensationClocks = roundU32(compensationTime * (float)DDA::stepClockRate * params.compFactor);

	// Calculate the net total step count to allow for compensation. It may be negative.
	const float compensationDistance = (dda.endSpeed - dda.startSpeed) * compensationTime;
	const int32_t netSteps = (int32_t)(compensationDistance * stepsPerMm) + (int32_t)totalSteps;

	// Calculate the acceleration phase parameters
	const float accelCompensationDistance = compensationTime * (dda.topSpeed - dda.startSpeed);

	// Acceleration phase parameters
	mp.cart.accelStopStep = (uint32_t)((dda.accelDistance + accelCompensationDistance) * stepsPerMm) + 1;

	// Constant speed phase parameters
	mp.cart.mmPerStepTimesCKdivtopSpeed = (uint32_t)((float)((uint64_t)DDA::stepClockRate * K1)/(stepsPerMm * dda.topSpeed));

	// Calculate the deceleration and reverse phase parameters
	// First check whether there is any deceleration at all, otherwise we may get strange results because of rounding errors
	if (dda.decelDistance * stepsPerMm < 0.5)		// if less than 1 deceleration step
	{
		totalSteps = (uint32_t)max<int32_t>(netSteps, 0);
		mp.cart.decelStartStep = reverseStartStep = netSteps + 1;
		mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA = 0;
		twoDistanceToStopTimesCsquaredDivA = 0;
	}
	else
	{
		mp.cart.decelStartStep = (uint32_t)((params.decelStartDistance + accelCompensationDistance) * stepsPerMm) + 1;
		const int32_t initialDecelSpeedTimesCdivA = (int32_t)params.topSpeedTimesCdivA - (int32_t)mp.cart.compensationClocks;	// signed because it may be negative and we square it
		const uint64_t initialDecelSpeedTimesCdivASquared = isquare64(initialDecelSpeedTimesCdivA);
		twoDistanceToStopTimesCsquaredDivA =
			initialDecelSpeedTimesCdivASquared + roundU64(((params.decelStartDistance + accelCompensationDistance) * (float)(DDA::stepClockRateSquared * 2))/dda.acceleration);

		// Calculate the move distance to the point of zero speed, where reverse motion starts
		const float initialDecelSpeed = dda.topSpeed - dda.acceleration * compensationTime;
		const float reverseStartDistance = (initialDecelSpeed > 0.0)
												? fsquare(initialDecelSpeed)/(2 * dda.acceleration) + params.decelStartDistance
												: params.decelStartDistance;
		// Reverse phase parameters
		if (reverseStartDistance >= dda.totalDistance)
		{
			// No reverse phase
			totalSteps = (uint32_t)max<int32_t>(netSteps, 0);
			reverseStartStep = netSteps + 1;
			mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA = 0;
		}
		else
		{
			reverseStartStep = (initialDecelSpeed < 0.0)
								? mp.cart.decelStartStep
								: (twoDistanceToStopTimesCsquaredDivA/mp.cart.twoCsquaredTimesMmPerStepDivA) + 1;
			// Because the step numbers are rounded down, we may sometimes get a situation in which netSteps = 1 and reverseStartStep = 1.
			// This would lead to totalSteps = -1, which must be avoided.
			const int32_t overallSteps = (int32_t)(2 * (reverseStartStep - 1)) - netSteps;
			if (overallSteps > 0)
			{
				totalSteps = (uint32_t)overallSteps;
				mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA =
						(int64_t)((2 * (reverseStartStep - 1)) * mp.cart.twoCsquaredTimesMmPerStepDivA) - (int64_t)twoDistanceToStopTimesCsquaredDivA;
			}
			else
			{
				totalSteps = (uint32_t)max<int32_t>(netSteps, 0);
				reverseStartStep = totalSteps + 1;
				mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA = 0;
			}
		}
	}
}

void DriveMovement::DebugPrint(char c, bool isDeltaMovement) const
{
	if (state != DMState::idle)
	{
		debugPrintf("DM%c%s dir=%c steps=%" PRIu32 " next=%" PRIu32 " rev=%" PRIu32 " interval=%" PRIu32
					" 2dtstc2diva=%" PRIu64 "\n",
					c, (state == DMState::stepError) ? " ERR:" : ":", (direction) ? 'F' : 'B', totalSteps, nextStep, reverseStartStep, stepInterval,
					twoDistanceToStopTimesCsquaredDivA);

		if (isDeltaMovement)
		{
			debugPrintf("hmz0sK=%" PRIi32 " minusAaPlusBbTimesKs=%" PRIi32 " dSquaredMinusAsquaredMinusBsquared=%" PRId64 "\n"
						"2c2mmsda=%" PRIu64 " asdsk=%" PRIu32 " dsdsk=%" PRIu32 " mmstcdts=%" PRIu32 "\n",
						mp.delta.hmz0sK, mp.delta.minusAaPlusBbTimesKs, mp.delta.dSquaredMinusAsquaredMinusBsquaredTimesKsquaredSsquared,
						mp.delta.twoCsquaredTimesMmPerStepDivA, mp.delta.accelStopDsK, mp.delta.decelStartDsK, mp.delta.mmPerStepTimesCKdivtopSpeed
						);
		}
		else
		{
			debugPrintf("accelStopStep=%" PRIu32 " decelStartStep=%" PRIu32 " 2CsqtMmPerStepDivA=%" PRIu64 "\n"
						"mmPerStepTimesCdivtopSpeed=%" PRIu32 " fmsdmtstdca2=%" PRId64 " cc=%" PRIu32 " acc=%" PRIu32 "\n",
						mp.cart.accelStopStep, mp.cart.decelStartStep, mp.cart.twoCsquaredTimesMmPerStepDivA,
						mp.cart.mmPerStepTimesCKdivtopSpeed, mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA, mp.cart.compensationClocks, mp.cart.accelCompensationClocks
						);
		}
	}
	else
	{
		debugPrintf("DM%c: not moving\n", c);
	}
}

// Calculate and store the time since the start of the move when the next step for the specified DriveMovement is due.
// Return true if there are more steps to do.
// This is also used for extruders on delta machines.
bool DriveMovement::CalcNextStepTimeCartesianFull(const DDA &dda, bool live)
pre(nextStep < totalSteps; stepsTillRecalc == 0)
{
	// Work out how many steps to calculate at a time.
	// The last step before reverseStartStep must be single stepped to make sure that we don't reverse the direction too soon.
	uint32_t shiftFactor = 0;		// assume single stepping
	if (stepInterval < DDA::MinCalcIntervalCartesian)
	{
		uint32_t stepsToLimit = ((nextStep <= reverseStartStep && reverseStartStep <= totalSteps)
									? reverseStartStep
									: totalSteps
								) - nextStep;
		if (stepInterval < DDA::MinCalcIntervalCartesian/4 && stepsToLimit > 8)
		{
			shiftFactor = 3;		// octal stepping
		}
		else if (stepInterval < DDA::MinCalcIntervalCartesian/2 && stepsToLimit > 4)
		{
			shiftFactor = 2;		// quad stepping
		}
		else if (stepsToLimit > 2)
		{
			shiftFactor = 1;		// double stepping
		}
	}

	stepsTillRecalc = (1u << shiftFactor) - 1u;					// store number of additional steps to generate

	const uint32_t nextCalcStep = nextStep + stepsTillRecalc;
	const uint32_t lastStepTime = nextStepTime;					// pick up the time of the last step
	if (nextCalcStep < mp.cart.accelStopStep)
	{
		// acceleration phase
		const uint32_t adjustedStartSpeedTimesCdivA = dda.startSpeedTimesCdivA + mp.cart.compensationClocks;
		nextStepTime = isqrt64(isquare64(adjustedStartSpeedTimesCdivA) + (mp.cart.twoCsquaredTimesMmPerStepDivA * nextCalcStep)) - adjustedStartSpeedTimesCdivA;
	}
	else if (nextCalcStep < mp.cart.decelStartStep)
	{
		// steady speed phase
		nextStepTime = (uint32_t)(  (int32_t)(((uint64_t)mp.cart.mmPerStepTimesCKdivtopSpeed * nextCalcStep)/K1)
								  + dda.extraAccelerationClocks
								  - (int32_t)mp.cart.accelCompensationClocks
								 );
	}
	else if (nextCalcStep < reverseStartStep)
	{
		// deceleration phase, not reversed yet
		const uint64_t temp = mp.cart.twoCsquaredTimesMmPerStepDivA * nextCalcStep;
		const uint32_t adjustedTopSpeedTimesCdivAPlusDecelStartClocks = dda.topSpeedTimesCdivAPlusDecelStartClocks - mp.cart.compensationClocks;
		// Allow for possible rounding error when the end speed is zero or very small
		nextStepTime = (temp < twoDistanceToStopTimesCsquaredDivA)
						? adjustedTopSpeedTimesCdivAPlusDecelStartClocks - isqrt64(twoDistanceToStopTimesCsquaredDivA - temp)
						: adjustedTopSpeedTimesCdivAPlusDecelStartClocks;
	}
	else
	{
		// deceleration phase, reversing or already reversed
		if (nextCalcStep == reverseStartStep)
		{
			direction = !direction;
			if (live)
			{
				reprap.GetPlatform().SetDirection(drive, direction);
			}
		}
		const uint32_t adjustedTopSpeedTimesCdivAPlusDecelStartClocks = dda.topSpeedTimesCdivAPlusDecelStartClocks - mp.cart.compensationClocks;
		nextStepTime = adjustedTopSpeedTimesCdivAPlusDecelStartClocks
							+ isqrt64((int64_t)(mp.cart.twoCsquaredTimesMmPerStepDivA * nextCalcStep) - mp.cart.fourMaxStepDistanceMinusTwoDistanceToStopTimesCsquaredDivA);
	}

	stepInterval = (nextStepTime - lastStepTime) >> shiftFactor;	// calculate the time per step, ready for next time

	if (nextStepTime > dda.clocksNeeded)
	{
		// The calculation makes this step late.
		// When the end speed is very low, calculating the time of the last step is very sensitive to rounding error.
		// So if this is the last step and it is late, bring it forward to the expected finish time.
		// Very rarely on a delta, the penultimate step may also be calculated late. Allow for that here in case it affects Cartesian axes too.
		if (nextStep + 1 >= totalSteps)
		{
			nextStepTime = dda.clocksNeeded;
		}
		else
		{
			// We don't expect any step except the last to be late
			state = DMState::stepError;
			stepInterval = 10000000 + nextStepTime;				// so we can tell what happened in the debug print
			return false;
		}
	}
	return true;
}

// Calculate the time since the start of the move when the next step for the specified DriveMovement is due
// Return true if there are more steps to do
bool DriveMovement::CalcNextStepTimeDeltaFull(const DDA &dda, bool live)
pre(nextStep < totalSteps; stepsTillRecalc == 0)
{
	// Work out how many steps to calculate at a time.
	// The last step before reverseStartStep must be single stepped to make sure that we don't reverse the direction too soon.
	// The simulator suggests that at 200steps/mm, the minimum step pulse interval for 400mm/sec movement is 4.5us
	uint32_t shiftFactor = 0;		// assume single stepping
	if (stepInterval < DDA::MinCalcIntervalDelta)
	{
		const uint32_t stepsToLimit = ((nextStep < reverseStartStep && reverseStartStep <= totalSteps)
										? reverseStartStep
										: totalSteps
									  ) - nextStep;
		if (stepInterval < DDA::MinCalcIntervalDelta/8 && stepsToLimit > 16)
		{
			shiftFactor = 4;		// hexadecimal stepping
		}
		else if (stepInterval < DDA::MinCalcIntervalDelta/4 && stepsToLimit > 8)
		{
			shiftFactor = 3;		// octal stepping
		}
		else if (stepInterval < DDA::MinCalcIntervalDelta/2 && stepsToLimit > 4)
		{
			shiftFactor = 2;		// quad stepping
		}
		else if (stepsToLimit > 2)
		{
			shiftFactor = 1;		// double stepping
		}
	}

	stepsTillRecalc = (1u << shiftFactor) - 1;					// store number of additional steps to generate

	if (nextStep == reverseStartStep)
	{
		direction = false;
		if (live)
		{
			reprap.GetPlatform().SetDirection(drive, false);	// going down now
		}
	}

	// Calculate d*s*K as an integer, where d = distance the head has travelled, s = steps/mm for this drive, K = a power of 2 to reduce the rounding errors
	{
		int32_t shiftedK2 = (int32_t)(K2 << shiftFactor);
		if (!direction)
		{
			shiftedK2 = -shiftedK2;
		}
		mp.delta.hmz0sK += shiftedK2;
	}

	const int32_t hmz0scK = (int32_t)(((int64_t)mp.delta.hmz0sK * dda.cKc)/Kc);
	const int32_t t1 = mp.delta.minusAaPlusBbTimesKs + hmz0scK;
	// Due to rounding error we can end up trying to take the square root of a negative number if we do not take precautions here
	const int64_t t2a = mp.delta.dSquaredMinusAsquaredMinusBsquaredTimesKsquaredSsquared - (int64_t)isquare64(mp.delta.hmz0sK) + (int64_t)isquare64(t1);
	const int32_t t2 = (t2a > 0) ? isqrt64(t2a) : 0;
	const int32_t dsK = (direction) ? t1 - t2 : t1 + t2;

	// Now feed dsK into a modified version of the step algorithm for Cartesian motion without elasticity compensation
	if (dsK < 0)
	{
		state = DMState::stepError;
		nextStep += 1000000;						// so that we can tell what happened in the debug print
		return false;
	}

	const uint32_t lastStepTime = nextStepTime;		// pick up the time of the last step
	if ((uint32_t)dsK < mp.delta.accelStopDsK)
	{
		// Acceleration phase
		nextStepTime = isqrt64(isquare64(dda.startSpeedTimesCdivA) + (mp.delta.twoCsquaredTimesMmPerStepDivA * (uint32_t)dsK)/K2) - dda.startSpeedTimesCdivA;
	}
	else if ((uint32_t)dsK < mp.delta.decelStartDsK)
	{
		// Steady speed phase
		nextStepTime = (uint32_t)(  (int32_t)(((uint64_t)mp.delta.mmPerStepTimesCKdivtopSpeed * (uint32_t)dsK)/(K1 * K2))
								  + dda.extraAccelerationClocks
								 );
	}
	else
	{
		const uint64_t temp = (mp.delta.twoCsquaredTimesMmPerStepDivA * (uint32_t)dsK)/K2;
		// Because of possible rounding error when the end speed is zero or very small, we need to check that the square root will work OK
		nextStepTime = (temp < twoDistanceToStopTimesCsquaredDivA)
						? dda.topSpeedTimesCdivAPlusDecelStartClocks - isqrt64(twoDistanceToStopTimesCsquaredDivA - temp)
						: dda.topSpeedTimesCdivAPlusDecelStartClocks;
	}

	stepInterval = (nextStepTime - lastStepTime) >> shiftFactor;	// calculate the time per step, ready for next time

	if (nextStepTime > dda.clocksNeeded)
	{
		// The calculation makes this step late.
		// When the end speed is very low, calculating the time of the last step is very sensitive to rounding error.
		// So if this is the last step and it is late, bring it forward to the expected finish time.
		// Very rarely, the penultimate step may be calculated late, so allow for that too.
		if (nextStep + 1 >= totalSteps)
		{
			nextStepTime = dda.clocksNeeded;
		}
		else
		{
			// We don't expect any steps except the last two to be late
			state = DMState::stepError;
			stepInterval = 10000000 + nextStepTime;		// so we can tell what happened in the debug print
			return false;
		}
	}
	return true;
}

// Reduce the speed of this movement. Called to reduce the homing speed when we detect we are near the endstop for a drive.
void DriveMovement::ReduceSpeed(const DDA& dda, uint32_t inverseSpeedFactor)
{
	if (dda.isDeltaMovement)
	{
		// Force the linear motion phase
		mp.delta.accelStopDsK = 0;
		mp.delta.decelStartDsK = 0xFFFFFFFF;

		// Adjust the speed
		mp.delta.mmPerStepTimesCKdivtopSpeed *= inverseSpeedFactor;
	}
	else
	{
		// Force the linear motion phase
		mp.cart.accelStopStep = 0;
		mp.cart.decelStartStep = totalSteps + 1;

		// Adjust the speed
		mp.cart.mmPerStepTimesCKdivtopSpeed *= inverseSpeedFactor;
	}
}

// End
