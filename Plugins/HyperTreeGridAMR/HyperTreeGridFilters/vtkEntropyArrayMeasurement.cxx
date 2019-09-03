/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkEntropyArrayMeasurement.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkEntropyArrayMeasurement.h"

#include "vtkArithmeticAccumulator.h"
#include "vtkEntropyAccumulator.h"

#include <cassert>
#include <cmath>

vtkStandardNewMacro(vtkEntropyArrayMeasurement);

//----------------------------------------------------------------------------
vtkEntropyArrayMeasurement::vtkEntropyArrayMeasurement()
{
  this->Accumulators.resize(2);
  this->Accumulators[0] = vtkArithmeticAccumulator::New();
  this->Accumulators[1] = vtkEntropyAccumulator::New();
}

//----------------------------------------------------------------------------
double vtkEntropyArrayMeasurement::Measure() const
{
  assert(this->Accumulators.size() > 1 && "No accumulator, cannot measure");
  // x_i : input
  // p_i = f_i / n
  // entropy = 1/n sum_i (x_i log(x_i) + x_i log(n))
  return (this->Accumulators[0]->GetValue() * std::log(this->NumberOfAccumulatedData) +
           this->Accumulators[1]->GetValue()) /
    this->NumberOfAccumulatedData;
}

//----------------------------------------------------------------------------
void vtkEntropyArrayMeasurement::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
