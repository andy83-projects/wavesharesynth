#include "rosic_AnalogEnvelope.h"
using namespace rosic;

//-------------------------------------------------------------------------------------------------
// construction/destruction:

AnalogEnvelope::AnalogEnvelope()
{
  sampleRate     = 44100.0f;
  startLevel     = 0.0f;
  attackTime     = 0.0f;
  peakLevel      = 1.0f;
  holdTime       = 0.0f;
  decayTime      = 0.1f;
  sustainLevel   = 0.5f;
  releaseTime    = 0.01f;
  endLevel       = 0.0f;
  time           = 0.0f;
  timeScale      = 1.0f;
  peakByVel      = 1.0f;
  peakByKey      = 1.0f;
  timeScaleByVel = 1.0f;
  timeScaleByKey = 1.0f;
  increment      = 1000.0f*timeScale/sampleRate;
  tauScale       = 1.0f;
  peakScale      = 1.0f;
  noteIsOn       = false;
  outputIsZero   = true;

  previousOutput = 0.0f;

  // call these functions to trigger the coefficient calculations:
  setAttack(attackTime);
  setDecay(decayTime);
  setRelease(releaseTime);
}

AnalogEnvelope::~AnalogEnvelope()
{

}

//-------------------------------------------------------------------------------------------------
// parameter settings:

void AnalogEnvelope::setSampleRate(float newSampleRate)
{
  if( newSampleRate > 0.0f )
    sampleRate = newSampleRate;

  // adjust time increment:
  increment = 1000.0f*timeScale/sampleRate;

  //re-calculate coefficients for the 3 filters:
  setAttack (attackTime);
  setDecay  (decayTime);
  setRelease(releaseTime);
}

void AnalogEnvelope::setAttack(float newAttackTime)
{
  if( newAttackTime > 0.0f )
  {
    attackTime  = newAttackTime;
    float tau  = (sampleRate*0.001f*attackTime) * tauScale/timeScale;
    attackCoeff = 1.0f - exp( -1.0f / tau );
  }
  else // newAttackTime <= 0
  {
    attackTime  = 0.0f;
    attackCoeff = 1.0f;
  }
  calculateAccumulatedTimes();
}

void AnalogEnvelope::setHold(float newHoldTime)
{
  if( newHoldTime >= 0 )
    holdTime = newHoldTime;
  calculateAccumulatedTimes();
}

void AnalogEnvelope::setDecay(float newDecayTime)
{
  if( newDecayTime > 0.0f )
  {
    decayTime  = newDecayTime;
    float tau = (sampleRate*0.001f*decayTime) * tauScale/timeScale;
    decayCoeff = 1.0f - exp( -1.0f / tau  );
  }
  else // newDecayTime <= 0
  {
    decayTime  = 0.0f;
    decayCoeff = 1.0f;
  }
  calculateAccumulatedTimes();
}

void AnalogEnvelope::setRelease(float newReleaseTime)
{
  if( newReleaseTime > 0.0f )
  {
    releaseTime  = newReleaseTime;
    float tau   = (sampleRate*0.001f*releaseTime) * tauScale/timeScale;
    releaseCoeff = 1.0f - exp( -1.0f / tau  );
  }
  else // newReleaseTime <= 0
  {
    releaseTime  = 0.0f;
    releaseCoeff = 1.0f;
  }
  calculateAccumulatedTimes();
}

void AnalogEnvelope::setTimeScale(float newTimeScale)
{
  if( newTimeScale > 0 )
    timeScale = newTimeScale;

  increment  = 1000.0f*timeScale/sampleRate;

  //re-calculate coefficients for the 3 filters:
  setAttack (attackTime);
  setDecay  (decayTime);
  setRelease(releaseTime);
}

void AnalogEnvelope::setTauScale(float newTauScale)
{
  if( newTauScale > 0 )
    tauScale = newTauScale;

  setAttack(attackTime);
  setDecay(decayTime);
  setRelease(releaseTime);
}

void AnalogEnvelope::setPeakScale(float newPeakScale)
{
  if( newPeakScale > 0 )
    peakScale = newPeakScale;
}

//-------------------------------------------------------------------------------------------------
// others:

void AnalogEnvelope::reset()
{
  time = 0.0f;
}

void AnalogEnvelope::noteOn(bool startFromCurrentLevel, int newKey, int newVel)
{
  if( !startFromCurrentLevel )
    previousOutput = startLevel;  // may lead to clicks


  // \todo: calculate key and velocity scale factors for duration and peak-value...


  // reset time for the new note:
  time         = 0.0f;
  noteIsOn     = true;
  outputIsZero = false;
}

void AnalogEnvelope::noteOff()
{
  noteIsOn = false;

  // advance time to the beginnig of the release phase:
  time = (attackTime + holdTime + decayTime + increment);
}

bool AnalogEnvelope::endIsReached()
{
  //return false; // test

  if( noteIsOn == false && previousOutput < 0.000001f )
    return true;
  else
    return false;
}

//-------------------------------------------------------------------------------------------------
// internal functions:

void AnalogEnvelope::calculateAccumulatedTimes()
{
  attPlusHld               = attackTime + holdTime;
  attPlusHldPlusDec        = attPlusHld + decayTime;
  attPlusHldPlusDecPlusRel = attPlusHldPlusDec + releaseTime;
}
