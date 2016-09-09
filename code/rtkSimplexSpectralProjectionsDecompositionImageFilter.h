/*=========================================================================
 *
 *  Copyright RTK Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

#ifndef rtkSimplexSpectralProjectionsDecompositionImageFilter_h
#define rtkSimplexSpectralProjectionsDecompositionImageFilter_h

#include <itkImageToImageFilter.h>
#include <itkAmoebaOptimizer.h>

namespace rtk
{
  /** \class SimplexSpectralProjectionsDecompositionImageFilter
   * \brief Decomposition of spectral projection images into material projections
   *
   * See the reference paper: "Experimental feasibility of multi-energy photon-counting
   * K-edge imaging in pre-clinical computed tomography", Schlomka et al, PMB 2008
   *
   * \author Cyril Mory
   *
   * \ingroup ReconstructionAlgorithm
   */

// We have to define the cost function first
template <unsigned int NbMaterials = 3, unsigned int NumberOfSpectralBins = 6, unsigned int NumberOfEnergies = 150>
class Schlomka2008NegativeLogLikelihood : public itk::SingleValuedCostFunction
{
public:

  typedef Schlomka2008NegativeLogLikelihood   Self;
  typedef itk::SingleValuedCostFunction       Superclass;
  typedef itk::SmartPointer<Self>             Pointer;
  typedef itk::SmartPointer<const Self>       ConstPointer;
  itkNewMacro( Self );
  itkTypeMacro( Schlomka2008NegativeLogLikelihood, SingleValuedCostFunction );

  enum { SpaceDimension=NbMaterials };

  typedef Superclass::ParametersType      ParametersType;
  typedef Superclass::DerivativeType      DerivativeType;
  typedef Superclass::MeasureType         MeasureType;

  typedef itk::Matrix<float, NumberOfSpectralBins, NumberOfEnergies>            DetectorResponseType;
  typedef itk::Vector<itk::Vector<float, NumberOfEnergies>, NbMaterials>        MaterialAttenuationsType;
  typedef itk::Vector<float, NumberOfSpectralBins>                              DetectorCountsType;
  typedef itk::Vector<float, NumberOfEnergies>                                  IncidentSpectrumType;

  // Constructor
  Schlomka2008NegativeLogLikelihood()
  {
  }

  // Destructor
  ~Schlomka2008NegativeLogLikelihood()
  {
  }

  itk::Vector<float, NumberOfSpectralBins> ForwardModel(const ParametersType & lineIntegrals) const
  {
  // Apply detector response, getting the lambdas
  return m_DetectorResponse * GetAttenuatedIncidentSpectrum(lineIntegrals);
  }

  itk::Vector<float, NumberOfEnergies> GetAttenuatedIncidentSpectrum(const ParametersType & lineIntegrals) const
  {
  // Solid angle of detector pixel, exposure time and mAs should already be
  // taken into account in the incident spectrum image

  // Apply attenuation at each energy
  itk::Vector<float, NumberOfEnergies> attenuatedIncidentSpectrum;
  attenuatedIncidentSpectrum.Fill(0);
  for (unsigned int e=0; e<NumberOfEnergies; e++)
    {
    float totalAttenuation = 0.;
    for (unsigned int m=0; m<NbMaterials; m++)
      {
      totalAttenuation += lineIntegrals[m] * m_MaterialAttenuations[m][e];
      }

    attenuatedIncidentSpectrum[e] = m_IncidentSpectrum[e] * std::exp(-totalAttenuation);
    }

  return attenuatedIncidentSpectrum;
  }

  itk::Vector<float, NbMaterials> GetCramerRaoLowerBound(const ParametersType & lineIntegrals) const
  {
  // Get some required data
  itk::Vector<float, NumberOfEnergies> attenuatedIncidentSpectrum = GetAttenuatedIncidentSpectrum(lineIntegrals);
  itk::Vector<float, NumberOfSpectralBins> lambdas = ForwardModel(lineIntegrals);

  // Compute the vector of m_b / lambda_b²
  itk::Vector<float, NumberOfSpectralBins> weights;
  weights.SetVnlVector(element_product(lambdas.GetVnlVector(), lambdas.GetVnlVector()));
  weights.SetVnlVector(element_product(weights.GetVnlVector(), m_DetectorCounts.GetVnlVector()));

  // Prepare intermediate variables
  itk::Vector<float, NumberOfEnergies> intermediate_a;
  itk::Vector<float, NumberOfEnergies> intermediate_a_prime;
  itk::Vector<float, NumberOfSpectralBins> partial_derivative_a;
  itk::Vector<float, NumberOfSpectralBins> partial_derivative_a_prime;

  // Compute the Fischer information matrix
  itk::Matrix<float, NbMaterials, NbMaterials> Fischer;
  for (unsigned int a=0; a<NbMaterials; a++)
    {
    for (unsigned int a_prime=0; a_prime<NbMaterials; a_prime++)
      {
      // Compute the partial derivatives of lambda_b with respect to the material line integrals
      intermediate_a.SetVnlVector(element_product(attenuatedIncidentSpectrum.GetVnlVector(), m_MaterialAttenuations[a].GetVnlVector()));
      intermediate_a_prime.SetVnlVector(element_product(attenuatedIncidentSpectrum.GetVnlVector(), m_MaterialAttenuations[a_prime].GetVnlVector()));

      partial_derivative_a = m_DetectorResponse * intermediate_a;
      partial_derivative_a_prime = m_DetectorResponse * intermediate_a_prime;

      // Multiply them together element-wise, then dot product with the weights
      partial_derivative_a_prime.SetVnlVector(element_product(partial_derivative_a.GetVnlVector(), partial_derivative_a_prime.GetVnlVector()));
      Fischer[a][a_prime] = partial_derivative_a_prime * weights;
      }
    }

  // Invert the Fischer matrix and extract the diagonal components (variances)
  itk::Vector<float, NbMaterials> diag = Fischer.GetInverse().get_diagonal();

  // Return the inverse (element wise) of the diagonal components (inverse variances, to be used directly in WLS reconstruction)
  for (unsigned int mat=0; mat<NbMaterials; mat++)
    diag[mat] = 1./diag[mat];
  return diag;
  }

  // Not implemented, since it is too complex to compute
  // Therefore we will only use a zero-th order method
  void GetDerivative( const ParametersType & ,
                      DerivativeType &  ) const
  {
  }

  // Main method
  MeasureType  GetValue( const ParametersType & parameters ) const
  {
  // Forward model: compute the expected number of counts in each bin
  DetectorCountsType lambdas = ForwardModel(parameters);

  // Compute the negative log likelihood from the lambdas
  MeasureType measure = 0;
  for (unsigned int i=0; i<NumberOfSpectralBins; i++)
    measure += lambdas[i] - std::log(lambdas[i]) * m_DetectorCounts[i];

  return measure;
  }

  unsigned int GetNumberOfParameters(void) const
  {
  return SpaceDimension;
  }

  itkSetMacro(DetectorCounts, DetectorCountsType)
  itkGetMacro(DetectorCounts, DetectorCountsType)

  itkSetMacro(DetectorResponse, DetectorResponseType)
  itkGetMacro(DetectorResponse, DetectorResponseType)

  itkSetMacro(IncidentSpectrum, IncidentSpectrumType)
  itkGetMacro(IncidentSpectrum, IncidentSpectrumType)

  itkSetMacro(MaterialAttenuations, MaterialAttenuationsType)
  itkGetMacro(MaterialAttenuations, MaterialAttenuationsType)

protected:
  MaterialAttenuationsType    m_MaterialAttenuations;
  DetectorResponseType        m_DetectorResponse;
  IncidentSpectrumType        m_IncidentSpectrum;
  DetectorCountsType          m_DetectorCounts;

private:
  Schlomka2008NegativeLogLikelihood(const Self &); //purposely not implemented
  void operator = (const Self &); //purposely not implemented

};


template<typename DecomposedProjectionsType, typename SpectralProjectionsType,
         unsigned int NumberOfEnergies = 150, typename IncidentSpectrumImageType = itk::Image<itk::Vector<float, NumberOfEnergies>, 2>,
         typename DetectorResponseImageType = itk::Image<float, 2>, typename MaterialAttenuationsImageType = itk::Image<float, 2> >
class ITK_EXPORT SimplexSpectralProjectionsDecompositionImageFilter :
  public itk::ImageToImageFilter<DecomposedProjectionsType, DecomposedProjectionsType>
{
public:
  /** Standard class typedefs. */
  typedef SimplexSpectralProjectionsDecompositionImageFilter                                Self;
  typedef itk::ImageToImageFilter<DecomposedProjectionsType, DecomposedProjectionsType>     Superclass;
  typedef itk::SmartPointer<Self>                                                           Pointer;
  typedef itk::SmartPointer<const Self>                                                     ConstPointer;

  /** Some convenient typedefs. */
  typedef SpectralProjectionsType       InputImageType;
  typedef SpectralProjectionsType       OutputImageType;

  /** Convenient information */
  typedef itk::Matrix<float, SpectralProjectionsType::PixelType::Dimension, NumberOfEnergies>                 DetectorResponseType;
  typedef itk::Vector<itk::Vector<float, NumberOfEnergies>, DecomposedProjectionsType::PixelType::Dimension>  MaterialAttenuationsType;
  typedef itk::Vector<unsigned int, SpectralProjectionsType::PixelType::Dimension + 1>                        ThresholdsType;

  /** Typedefs of each subfilter of this composite filter */
  typedef Schlomka2008NegativeLogLikelihood<DecomposedProjectionsType::PixelType::Dimension,
                                            SpectralProjectionsType::PixelType::Dimension, NumberOfEnergies>  CostFunctionType;

  /** Standard New method. */
  itkNewMacro(Self)

  /** Runtime information support. */
  itkTypeMacro(SimplexSpectralProjectionsDecompositionImageFilter, itk::ImageToImageFilter)

  /** Get / Set the number of iterations. Default is 300. */
  itkGetMacro(NumberOfIterations, unsigned int)
  itkSetMacro(NumberOfIterations, unsigned int)

  /** Set/Get the input material-decomposed stack of projections (only used for initialization) */
  void SetInputDecomposedProjections(const DecomposedProjectionsType* DecomposedProjections);
  typename DecomposedProjectionsType::ConstPointer GetInputDecomposedProjections();

  /** Set/Get the input stack of spectral projections (to be decomposed in materials) */
  void SetInputSpectralProjections(const SpectralProjectionsType* SpectralProjections);
  typename SpectralProjectionsType::ConstPointer GetInputSpectralProjections();

  /** Set/Get the incident spectrum input image */
  void SetInputIncidentSpectrum(const IncidentSpectrumImageType* IncidentSpectrum);
  typename IncidentSpectrumImageType::ConstPointer GetInputIncidentSpectrum();

  /** Set/Get the detector response as an image */
  void SetDetectorResponse(const DetectorResponseImageType* DetectorResponse);
  typename DetectorResponseImageType::ConstPointer GetDetectorResponse();

  /** Set/Get the material attenuations as an image */
  void SetMaterialAttenuations(const MaterialAttenuationsImageType* MaterialAttenuations);
  typename MaterialAttenuationsImageType::ConstPointer GetMaterialAttenuations();

  itkSetMacro(Thresholds, ThresholdsType)
  itkGetMacro(Thresholds, ThresholdsType)

protected:
  SimplexSpectralProjectionsDecompositionImageFilter();
  ~SimplexSpectralProjectionsDecompositionImageFilter() ITK_OVERRIDE {}

  void GenerateOutputInformation() ITK_OVERRIDE;

  void GenerateInputRequestedRegion() ITK_OVERRIDE;

  void BeforeThreadedGenerateData() ITK_OVERRIDE;
  void ThreadedGenerateData(const typename DecomposedProjectionsType::RegionType& outputRegionForThread, itk::ThreadIdType itkNotUsed(threadId)) ITK_OVERRIDE;

  /** The two inputs should not be in the same space so there is nothing
   * to verify. */
  void VerifyInputInformation() ITK_OVERRIDE {}

  MaterialAttenuationsType   m_MaterialAttenuations;
  DetectorResponseType       m_DetectorResponse;
  ThresholdsType             m_Thresholds;

  /** Number of simplex iterations */
  unsigned int m_NumberOfIterations;

private:
  //purposely not implemented
  SimplexSpectralProjectionsDecompositionImageFilter(const Self&);
  void operator=(const Self&);

}; // end of class

} // end namespace rtk


#ifndef ITK_MANUAL_INSTANTIATION
#include "rtkSimplexSpectralProjectionsDecompositionImageFilter.hxx"
#endif

#endif
