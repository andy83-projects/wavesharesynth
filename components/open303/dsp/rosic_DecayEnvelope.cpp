#include "rosic_DecayEnvelope.h"
using namespace rosic;

//-------------------------------------------------------------------------------------------------
// construction/destruction:

DecayEnvelope::DecayEnvelope()
{
  c            = 1.0f;
  y            = 1.0f;
  yInit        = 1.0f;
  tau          = 200.0f;
  fs           = 44100.0f;
  normalizeSum = false;
  calculateCoefficient();
}

DecayEnvelope::~DecayEnvelope()
{

}

//-------------------------------------------------------------------------------------------------
// parameter settings:

void DecayEnvelope::setSampleRate(float newSampleRate)
{
  if( newSampleRate > 0.0f )
  {
    fs = newSampleRate;
    calculateCoefficient();
  }
}

void DecayEnvelope::setDecayTimeConstant(float newTimeConstant)
{
  if( newTimeConstant > 0.001f ) // at least 0.001f ms decay
  {
    tau = newTimeConstant;
    calculateCoefficient();
  }
}

void DecayEnvelope::setNormalizeSum(bool shouldNormalizeSum)
{
  normalizeSum = shouldNormalizeSum;
  calculateCoefficient();
}

//-------------------------------------------------------------------------------------------------
// others:

void DecayEnvelope::trigger()
{
  y = yInit;
}

bool DecayEnvelope::endIsReached(float threshold)
{
  if( y < threshold )
    return true;
  else
    return false;
}

//-------------------------------------------------------------------------------------------------
// internal functions:

void DecayEnvelope::calculateCoefficient()
{
  c     = exp( -1.0f / (0.001f*tau*fs) );
  if( normalizeSum == true )
    yInit = (1.0f-c)/c;
  else
    yInit = 1.0f/c;
}
