#include "rosic_BlendOscillator.h"
using namespace rosic;

//-------------------------------------------------------------------------------------------------
// construction/destruction:

BlendOscillator::BlendOscillator()
{
  // init member variables:
  tableLengthDbl       = (float) MipMappedWaveTable::tableLength;  // typecasted version
  sampleRate           = 44100.0f;
  freq                 = 440.0f;
  increment            = (tableLengthDbl*freq)/sampleRate;
  phaseIndex           = 0.0f;
  startIndex           = 0.0f;
  waveTable1           = NULL;
  waveTable2           = NULL;

  // somewhat redundant:
  setSampleRate(44100.0f);          // sampleRate = 44100 Hz by default
  setFrequency (440.0f);            // frequency = 440 Hz by default
  setStartPhase(0.0f);              // sartPhase = 0 by default

  setWaveForm1(MipMappedWaveTable::SAW);
  setWaveForm2(MipMappedWaveTable::SQUARE);

  resetPhase();
}

BlendOscillator::~BlendOscillator()
{

}

//-------------------------------------------------------------------------------------------------
// parameter settings:

void BlendOscillator::setSampleRate(float newSampleRate)
{
  if( newSampleRate > 0.0f )
    sampleRate = newSampleRate;
  sampleRateRec = 1.0f / sampleRate;
  increment = tableLengthDbl*freq*sampleRateRec;
}

void BlendOscillator::setWaveForm1(int newWaveForm1)
{
  if( waveTable1 != NULL )
    waveTable1->setWaveform(newWaveForm1);
}

void BlendOscillator::setWaveForm2(int newWaveForm2)
{
  if( waveTable2 != NULL )
    waveTable2->setWaveform(newWaveForm2);
}

void BlendOscillator::setWaveTable1(MipMappedWaveTable* newWaveTable1)
{
  waveTable1 = newWaveTable1;
}

void BlendOscillator::setWaveTable2(MipMappedWaveTable* newWaveTable2)
{
  waveTable2 = newWaveTable2;
}

void BlendOscillator::setStartPhase(float StartPhase)
{
  if( (StartPhase>=0) && (StartPhase<=360) )
    startIndex = (StartPhase/360.0f)*tableLengthDbl;
}

//-------------------------------------------------------------------------------------------------
// event processing:

void BlendOscillator::resetPhase()
{
  phaseIndex = startIndex;
}

void BlendOscillator::setPhase(float PhaseIndex)
{
  phaseIndex = startIndex+PhaseIndex;
}
