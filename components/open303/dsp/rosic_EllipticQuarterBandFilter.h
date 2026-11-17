#ifndef rosic_EllipticQuarterBandFilter_h
#define rosic_EllipticQuarterBandFilter_h

#include <string.h> // for memmove

// rosic-indcludes:
#include "GlobalDefinitions.h"

namespace rosic
{

  /**

  This is an elliptic subband filter of 12th order using a Direct Form II implementation structure.

  */

  class EllipticQuarterBandFilter
  {

  public:

    //---------------------------------------------------------------------------------------------
    // construction/destruction:

    /** Constructor. */
    EllipticQuarterBandFilter();

    //---------------------------------------------------------------------------------------------
    // parameter settings:

    /** Resets the filter state. */
    void reset();

    //---------------------------------------------------------------------------------------------
    // audio processing:

    /** Calculates a single filtered output-sample. */
    INLINE float getSample(float in);

    //=============================================================================================

  protected:

    // state buffer:
    float w[12];

  };

  //-----------------------------------------------------------------------------------------------
  // inlined functions:

  INLINE float EllipticQuarterBandFilter::getSample(float in)
  {
    /* Original transfer function factored into six stable SOS sections. */
    static const float sos[6][5] = {
      {0.000136717321f, 0.000186958926f, 0.000136717321f, -1.58631166f, 0.642820281f},
      {1.0f, -0.371403547f, 1.0f, -1.56209572f, 0.716157088f},
      {1.0f, -1.02982428f, 1.0f, -1.53107009f, 0.813095166f},
      {1.0f, -1.26764039f, 1.0f, -1.50809400f, 0.893158218f},
      {1.0f, -1.36287948f, 1.0f, -1.49832655f, 0.947528817f},
      {1.0f, -1.39802472f, 1.0f, -1.50326245f, 0.984374740f}
    };
    for(unsigned i = 0; i < 6; ++i) {
      const float *c = sos[i];
      const float state = in - c[3] * w[2*i] - c[4] * w[2*i+1];
      in = c[0] * state + c[1] * w[2*i] + c[2] * w[2*i+1];
      w[2*i+1] = w[2*i];
      w[2*i] = state;
    }
    return in;
#if 0 /* Original direct-form coefficients retained as the factoring reference. */
    const float a01 =   -9.1891604652189471f;
    const float a02 =   40.177553696870497f;
    const float a03 = -110.11636661771178f;
    const float a04 =  210.18506612078195f;
    const float a05 = -293.84744771903240f;
    const float a06 =  308.16345558359234f;
    const float a07 = -244.06786780384243f;
    const float a08 =  144.81877911392738f;
    const float a09 =  -62.770692151724198f;
    const float a10 =   18.867762095902137f;
    const float a11 =   -3.5327094230551848f;
    const float a12 =    0.31183189275203149f;

    const float b00 =    0.00013671732099945628f;
    const float b01 =   -0.00055538501265606384f;
    const float b02 =    0.0013681887636296387f;
    const float b03 =   -0.0022158566490711852f;
    const float b04 =    0.0028320091007278322f;
    const float b05 =   -0.0029776933151090413f;
    const float b06 =    0.0030283628243514991f;
    const float b07 =   -0.0029776933151090413f;
    const float b08 =    0.0028320091007278331f;
    const float b09 =   -0.0022158566490711861f;
    const float b10 =    0.0013681887636296393f;
    const float b11 =   -0.00055538501265606384f;
    const float b12 =    0.00013671732099945636f;

    // calculate intermediate and output sample via direct form II - the parentheses facilitate
    // out-of-order execution of the independent additions (for performance optimization):
    float tmp =   (in + TINY)
                 - ( (a01*w[0] + a02*w[1] ) + (a03*w[2]  + a04*w[3]   ) )
                 - ( (a05*w[4] + a06*w[5] ) + (a07*w[6]  + a08*w[7]   ) )
                 - ( (a09*w[8] + a10*w[9] ) + (a11*w[10] +  a12*w[11] ) );

    float y =     b00*tmp
                 + ( (b01*w[0] + b02*w[1])  +  (b03*w[2]  + b04*w[3]  ) )
                 + ( (b05*w[4] + b06*w[5])  +  (b07*w[6]  + b08*w[7]  ) )
                 + ( (b09*w[8] + b10*w[9])  +  (b11*w[10] + b12*w[11] ) );

    // update state variables:
    memmove(&w[1], &w[0], 11*sizeof(float));
    w[0] = tmp;

    return y;
#endif
  }

} // end namespace rosic

#endif // rosic_EllipticQuarterBandFilter_h
