/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2014 Stanford University and the Authors.           *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "CudaMBPolKernels.h"
#include "CudaMBPolKernelSources.h"
#include "openmm/internal/ContextImpl.h"
#include "openmm/cuda/CudaBondedUtilities.h"
#include "openmm/cuda/CudaNonbondedUtilities.h"
#include "openmm/cuda/CudaForceInfo.h"
#include "CudaKernelSources.h"

//////////////// for electrostatics///////////////////////
#ifdef WIN32
  #define _USE_MATH_DEFINES // Needed to get M_PI
#endif
#include "openmm/internal/ContextImpl.h"
#include "openmm/internal/AmoebaMultipoleForceImpl.h"
#include "openmm/internal/NonbondedForceImpl.h"
#include "openmm/cuda/CudaBondedUtilities.h"
#include "openmm/cuda/CudaForceInfo.h"
#include "CudaKernelSources.h"
#include "openmm/cuda/CudaNonbondedUtilities.h"

#include <algorithm>
#include <cmath>
#ifdef _MSC_VER
#include <windows.h>
#endif
///////////////////////////////////////////////////////////////

using namespace MBPolPlugin;
using namespace OpenMM;
using namespace std;

class CudaMBPolOneBodyForceInfo : public CudaForceInfo {
public:
    CudaMBPolOneBodyForceInfo(const MBPolOneBodyForce& force) : force(force) {
    }
    int getNumParticleGroups() {
        return force.getNumOneBodys();
    }
    void getParticlesInGroup(int index, vector<int>& particles) {
        force.getOneBodyParameters(index, particles);
    }
    bool areGroupsIdentical(int group1, int group2) {
        vector<int> particleIndices;
        force.getOneBodyParameters(group1, particleIndices);
        force.getOneBodyParameters(group2, particleIndices);
        // there are no per-molecule parameters, so groups are always identical
        return true;
    }
private:
    const MBPolOneBodyForce& force;
};

CudaCalcMBPolOneBodyForceKernel::~CudaCalcMBPolOneBodyForceKernel() {
    cu.setAsCurrent();
    if (params != NULL)
        delete params;
}

void CudaCalcMBPolOneBodyForceKernel::initialize(const System& system, const MBPolOneBodyForce& force) {
    cu.setAsCurrent();
    int numContexts = cu.getPlatformData().contexts.size();
    int startIndex = cu.getContextIndex()*force.getNumOneBodys()/numContexts;
    int endIndex = (cu.getContextIndex()+1)*force.getNumOneBodys()/numContexts;
    numBonds = endIndex-startIndex;
    if (numBonds == 0)
        return;
    vector<vector<int> > atoms;
    for (int i = 0; i < numBonds; i++) {
        vector<int> particleIndices;
        force.getOneBodyParameters(startIndex+i, particleIndices);
        atoms.push_back(particleIndices);
    }
    cu.getBondedUtilities().addInteraction(atoms, CudaMBPolKernelSources::onebodyForce, force.getForceGroup());
    cu.addForce(new CudaMBPolOneBodyForceInfo(force));
}

double CudaCalcMBPolOneBodyForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy) {
    return 0.0;
}

void CudaCalcMBPolOneBodyForceKernel::copyParametersToContext(ContextImpl& context, const MBPolOneBodyForce& force) {
    cu.setAsCurrent();
    int numContexts = cu.getPlatformData().contexts.size();
    int startIndex = cu.getContextIndex()*force.getNumOneBodys()/numContexts;
    int endIndex = (cu.getContextIndex()+1)*force.getNumOneBodys()/numContexts;
    if (numBonds != endIndex-startIndex)
        throw OpenMMException("updateParametersInContext: The number of bonds has changed");
    if (numBonds == 0)
        return;
    
    // Mark that the current reordering may be invalid.
    
    cu.invalidateMolecules();
}

///////////////////////////////////////////// MBPolTwoBodyForce ////////////////////////////////////

class CudaMBPolTwoBodyForceInfo : public CudaForceInfo {
public:
    CudaMBPolTwoBodyForceInfo(const MBPolTwoBodyForce& force) : force(force) {
    }
    int getNumParticleGroups() {
        return force.getNumMolecules();
    }
    void getParticlesInGroup(int index, vector<int>& particles) {
        force.getParticleParameters(index, particles);
    }
    bool areGroupsIdentical(int group1, int group2) {
        return true;
    }
private:
    const MBPolTwoBodyForce& force;
};

CudaCalcMBPolTwoBodyForceKernel::CudaCalcMBPolTwoBodyForceKernel(std::string name, const Platform& platform, CudaContext& cu, const System& system) :
        CalcMBPolTwoBodyForceKernel(name, platform), cu(cu), system(system) {
}


CudaCalcMBPolTwoBodyForceKernel::~CudaCalcMBPolTwoBodyForceKernel() {
    cu.setAsCurrent();
}

void CudaCalcMBPolTwoBodyForceKernel::initialize(const System& system, const MBPolTwoBodyForce& force) {
    cu.setAsCurrent();

    // device array
    particleIndices = CudaArray::create<float4>(cu, cu.getPaddedNumAtoms(), "particleIndices");

    // suffix Vec is used for host arrays
    // FIXME forced to convert to float, otherwise type error in real_shfl
    // how to use ints?
    vector<float4> particleIndicesVec(cu.getPaddedNumAtoms());
    for (int i=0; i <  force.getNumMolecules(); i++) {
        std::vector<int> singleParticleIndices;
        force.getParticleParameters(i, singleParticleIndices );
        particleIndicesVec[i] = make_float4((float) singleParticleIndices[0], (float) singleParticleIndices[1], (float) singleParticleIndices[2], (float) singleParticleIndices[3]);
    }

    particleIndices->upload(particleIndicesVec);

    // a parameter is defined per mulecule
    // particleIndices as a parameter fails with an error on read_shfl
    cu.getNonbondedUtilities().addParameter(CudaNonbondedUtilities::ParameterInfo("particleIndices", "float", 4, sizeof(float4), particleIndices->getDevicePointer()));
    map<string, string> replacements;
    // replacements["PARAMS"] = cu.getNonbondedUtilities().addArgument(particleIndices->getDevicePointer(), "int4");
    
    // using an argument instead
   // posq is already on the device, format is float4 (x, y, z, charge) 
   // so, we can just pass as parameters the indices of the particles as we do
   // in the reference platform
   // after we copy params to the device
   //  replacements["PARAMS"] = cu.getBondedUtilities().addArgument(params->getDevicePointer(), "float2");
// 
   // then we also add another argument with the pointer to posq
   // the cu.getPosq().getDevicePointer()
   // so we can then access the position of all particles on the device
   //
   
    // replacements["POSQ"] = cu.getBondedUtilities().addArgument( cu.getPosq().getDevicePointer(), "float3");

    bool useCutoff = (force.getNonbondedMethod() != MBPolTwoBodyForce::NoCutoff);
    bool usePeriodic = (force.getNonbondedMethod() == MBPolTwoBodyForce::CutoffPeriodic);
    vector< vector<int> > exclusions;
    // cu.getNonbondedUtilities().addInteraction(useCutoff, usePeriodic, false, force.getCutoff(), exclusions, cu.replaceStrings(CudaMBPolKernelSources::twobodyForce, replacements), force.getForceGroup());
    // cu.addForce(new CudaMBPolTwoBodyForceInfo(force));
    
    // create an explicit CUDA kernel, this is necessary because we need access to
    // position and forces of all atoms in each molecule
    //
    map<string, string> defines;
    defines["NUM_ATOMS"] = cu.intToString(cu.getNumAtoms());
    defines["PADDED_NUM_ATOMS"] = cu.intToString(cu.getPaddedNumAtoms());
    defines["NUM_BLOCKS"] = cu.intToString(cu.getNumAtomBlocks());
    defines["TILE_SIZE"] = cu.intToString(CudaContext::TileSize);
    defines["THREAD_BLOCK_SIZE"] = cu.intToString(cu.getNonbondedUtilities().getNumForceThreadBlocks());
    //
    // tiles with exclusions setup
    
    int numContexts = cu.getPlatformData().contexts.size();
    // nb.initialize(system);
    // int numExclusionTiles = nb.getExclusionTiles().getSize();
    int numExclusionTiles = 1;

    defines["NUM_TILES_WITH_EXCLUSIONS"] = cu.intToString(numExclusionTiles);
    int startExclusionIndex = cu.getContextIndex()*numExclusionTiles/numContexts;
    int endExclusionIndex = (cu.getContextIndex()+1)*numExclusionTiles/numContexts;
        defines["FIRST_EXCLUSION_TILE"] = cu.intToString(startExclusionIndex);
    defines["LAST_EXCLUSION_TILE"] = cu.intToString(endExclusionIndex);
    // end of tiles with exclusions setup
    //
    if (useCutoff)
        defines["USE_CUTOFF"] = "1";
    double cutoff = force.getCutoff();
    defines["CUTOFF_SQUARED"] = cu.doubleToString(cutoff*cutoff);

    if (usePeriodic)
        defines["USE_PERIODIC"] = "1";

    CUmodule module = cu.createModule(CudaKernelSources::vectorOps+CudaMBPolKernelSources::multibodyLibrary + CudaMBPolKernelSources::twobodyForcePolynomial + CudaMBPolKernelSources::twobodyForce, defines);
    computeTwoBodyForceKernel = cu.getKernel(module, "computeTwoBodyForce");

    // Add an interaction to the default nonbonded kernel.  This doesn't actually do any calculations.  It's
    // just so that CudaNonbondedUtilities will build the exclusion flags and maintain the neighbor list.

    cu.getNonbondedUtilities().addInteraction(useCutoff, usePeriodic, false, force.getCutoff(), exclusions, "", force.getForceGroup());
    // cu.getNonbondedUtilities().setUsePadding(false);
    cu.addForce(new CudaMBPolTwoBodyForceInfo(force));

}

double CudaCalcMBPolTwoBodyForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy) {

    CudaNonbondedUtilities& nb = cu.getNonbondedUtilities();

    int startTileIndex = nb.getStartTileIndex();
    int numTileIndices = nb.getNumTiles();
    unsigned int maxTiles;
    if (nb.getUseCutoff()) {
        maxTiles = nb.getInteractingTiles().getSize();
    }

    void* args[] = {&cu.getForce().getDevicePointer(),
        &cu.getEnergyBuffer().getDevicePointer(),
        &cu.getPosq().getDevicePointer(),
        &nb.getExclusionTiles().getDevicePointer(),
        &startTileIndex,
        &numTileIndices,
        &cu.getNonbondedUtilities().getInteractingTiles().getDevicePointer(),
        &cu.getNonbondedUtilities().getInteractionCount().getDevicePointer(),
        cu.getPeriodicBoxSizePointer(),
        cu.getInvPeriodicBoxSizePointer(),
        &maxTiles,
       // &cu.getNonbondedUtilities().getBlock().getDevicePointer(),
        &cu.getNonbondedUtilities().getInteractingAtoms().getDevicePointer()
    };
    cu.executeKernel(computeTwoBodyForceKernel, args, cu.getPaddedNumAtoms());
    return 0.0;
}

void CudaCalcMBPolTwoBodyForceKernel::copyParametersToContext(ContextImpl& context, const MBPolTwoBodyForce& force) {
    cu.setAsCurrent();
    throw OpenMMException(" CudaCalcMBPolTwoBodyForceKernel::copyParametersToContext not implemented");
    
    cu.invalidateMolecules();
}




/* -------------------------------------------------------------------------- *
 *                             Electrostatics                                 *
 * -------------------------------------------------------------------------- */

class CudaCalcMBPolElectrostaticsForceKernel::ForceInfo : public CudaForceInfo {
public:
    ForceInfo(const AmoebaMultipoleForce& force) : force(force) {
    }
    bool areParticlesIdentical(int particle1, int particle2) {
        double charge1, charge2, thole1, thole2, damping1, damping2, polarity1, polarity2;
        int axis1, axis2, multipole11, multipole12, multipole21, multipole22, multipole31, multipole32;
        vector<double> dipole1, dipole2, quadrupole1, quadrupole2;
        force.getMultipoleParameters(particle1, charge1, dipole1, quadrupole1, axis1, multipole11, multipole21, multipole31, thole1, damping1, polarity1);
        force.getMultipoleParameters(particle2, charge2, dipole2, quadrupole2, axis2, multipole12, multipole22, multipole32, thole2, damping2, polarity2);
        if (charge1 != charge2 || thole1 != thole2 || damping1 != damping2 || polarity1 != polarity2 || axis1 != axis2) {
            return false;
        }
        for (int i = 0; i < (int) dipole1.size(); ++i) {
            if (dipole1[i] != dipole2[i]) {
                return false;
            }
        }
        for (int i = 0; i < (int) quadrupole1.size(); ++i) {
            if (quadrupole1[i] != quadrupole2[i]) {
                return false;
            }
        }
        return true;
    }
    int getNumParticleGroups() {
        return 7*force.getNumMultipoles();
    }
    void getParticlesInGroup(int index, vector<int>& particles) {
        int particle = index/7;
        int type = index-7*particle;
        force.getCovalentMap(particle, AmoebaMultipoleForce::CovalentType(type), particles);
    }
    bool areGroupsIdentical(int group1, int group2) {
        return ((group1%7) == (group2%7));
    }
private:
    const AmoebaMultipoleForce& force;
};

CudaCalcMBPolElectrostaticsForceKernel::CudaCalcMBPolElectrostaticsForceKernel(std::string name, const Platform& platform, CudaContext& cu, const System& system) :
		CalcMBPolElectrostaticsForceKernel(name, platform), cu(cu), system(system), hasInitializedScaleFactors(false), hasInitializedFFT(false), multipolesAreValid(false),
        multipoleParticles(NULL), molecularDipoles(NULL), molecularQuadrupoles(NULL), labFrameDipoles(NULL), labFrameQuadrupoles(NULL), fracDipoles(NULL),
        fracQuadrupoles(NULL), field(NULL), fieldPolar(NULL), inducedField(NULL), inducedFieldPolar(NULL), torque(NULL), dampingAndThole(NULL), inducedDipole(NULL),
        diisCoefficients(NULL), inducedDipolePolar(NULL), inducedDipoleErrors(NULL), prevDipoles(NULL), prevDipolesPolar(NULL), prevErrors(NULL), diisMatrix(NULL),
		polarizability(NULL), covalentFlags(NULL), polarizationGroupFlags(NULL), pmeGrid(NULL), pmeBsplineModuliX(NULL), pmeBsplineModuliY(NULL), pmeBsplineModuliZ(NULL),
		pmeIgrid(NULL), pmePhi(NULL), pmePhid(NULL), pmePhip(NULL), pmePhidp(NULL), pmeCphi(NULL), pmeAtomGridIndex(NULL), lastPositions(NULL), sort(NULL) {
}

CudaCalcMBPolElectrostaticsForceKernel::~CudaCalcMBPolElectrostaticsForceKernel() {
    cu.setAsCurrent();
    if (multipoleParticles != NULL)
        delete multipoleParticles;
    if (molecularDipoles != NULL)
        delete molecularDipoles;
    if (molecularQuadrupoles != NULL)
        delete molecularQuadrupoles;
    if (labFrameDipoles != NULL)
        delete labFrameDipoles;
    if (labFrameQuadrupoles != NULL)
        delete labFrameQuadrupoles;
    if (fracDipoles != NULL)
        delete fracDipoles;
    if (fracQuadrupoles != NULL)
        delete fracQuadrupoles;
    if (field != NULL)
        delete field;
    if (fieldPolar != NULL)
        delete fieldPolar;
    if (inducedField != NULL)
        delete inducedField;
    if (inducedFieldPolar != NULL)
        delete inducedFieldPolar;
    if (torque != NULL)
        delete torque;
    if (dampingAndThole != NULL)
        delete dampingAndThole;
    if (inducedDipole != NULL)
        delete inducedDipole;
    if (inducedDipolePolar != NULL)
        delete inducedDipolePolar;
    if (inducedDipoleErrors != NULL)
        delete inducedDipoleErrors;
    if (prevDipoles != NULL)
        delete prevDipoles;
    if (prevDipolesPolar != NULL)
        delete prevDipolesPolar;
    if (prevErrors != NULL)
        delete prevErrors;
    if (diisMatrix != NULL)
        delete diisMatrix;
    if (diisCoefficients != NULL)
        delete diisCoefficients;
    if (polarizability != NULL)
        delete polarizability;
    if (covalentFlags != NULL)
        delete covalentFlags;
    if (polarizationGroupFlags != NULL)
        delete polarizationGroupFlags;
    if (pmeGrid != NULL)
        delete pmeGrid;
    if (pmeBsplineModuliX != NULL)
        delete pmeBsplineModuliX;
    if (pmeBsplineModuliY != NULL)
        delete pmeBsplineModuliY;
    if (pmeBsplineModuliZ != NULL)
        delete pmeBsplineModuliZ;
    if (pmeIgrid != NULL)
        delete pmeIgrid;
    if (pmePhi != NULL)
        delete pmePhi;
    if (pmePhid != NULL)
        delete pmePhid;
    if (pmePhip != NULL)
        delete pmePhip;
    if (pmePhidp != NULL)
        delete pmePhidp;
    if (pmeCphi != NULL)
        delete pmeCphi;
    if (pmeAtomGridIndex != NULL)
        delete pmeAtomGridIndex;
    if (lastPositions != NULL)
        delete lastPositions;
    if (sort != NULL)
        delete sort;
    if (hasInitializedFFT)
        cufftDestroy(fft);
}

/**
 * Select a size for an FFT that is a multiple of 2, 3, 5, and 7.
 */
static int findFFTDimension(int minimum) {
    if (minimum < 1)
        return 1;
    while (true) {
        // Attempt to factor the current value.

        int unfactored = minimum;
        for (int factor = 2; factor < 8; factor++) {
            while (unfactored > 1 && unfactored%factor == 0)
                unfactored /= factor;
        }
        if (unfactored == 1)
            return minimum;
        minimum++;
    }
}

void CudaCalcMBPolElectrostaticsForceKernel::initialize(const System& system, const AmoebaMultipoleForce& force) {
    cu.setAsCurrent();

    // Initialize multipole parameters.

    numMultipoles = force.getNumMultipoles();
    CudaArray& posq = cu.getPosq();
    vector<double4> temp(posq.getSize());
    float4* posqf = (float4*) &temp[0];
    double4* posqd = (double4*) &temp[0];
    vector<float2> dampingAndTholeVec;
    vector<float> polarizabilityVec;
    vector<float> molecularDipolesVec;
    vector<float> molecularQuadrupolesVec;
    vector<int4> multipoleParticlesVec;
    for (int i = 0; i < numMultipoles; i++) {
        double charge, thole, damping, polarity;
        int axisType, atomX, atomY, atomZ;
        vector<double> dipole, quadrupole;
        force.getMultipoleParameters(i, charge, dipole, quadrupole, axisType, atomZ, atomX, atomY, thole, damping, polarity);
        if (cu.getUseDoublePrecision())
            posqd[i] = make_double4(0, 0, 0, charge);
        else
            posqf[i] = make_float4(0, 0, 0, (float) charge);
        dampingAndTholeVec.push_back(make_float2((float) damping, (float) thole));
        polarizabilityVec.push_back((float) polarity);
        multipoleParticlesVec.push_back(make_int4(atomX, atomY, atomZ, axisType));
        for (int j = 0; j < 3; j++)
            molecularDipolesVec.push_back((float) dipole[j]);
        molecularQuadrupolesVec.push_back((float) quadrupole[0]);
        molecularQuadrupolesVec.push_back((float) quadrupole[1]);
        molecularQuadrupolesVec.push_back((float) quadrupole[2]);
        molecularQuadrupolesVec.push_back((float) quadrupole[4]);
        molecularQuadrupolesVec.push_back((float) quadrupole[5]);
    }
    hasQuadrupoles = false;
    for (int i = 0; i < (int) molecularQuadrupolesVec.size(); i++)
        if (molecularQuadrupolesVec[i] != 0.0)
            hasQuadrupoles = true;
    int paddedNumAtoms = cu.getPaddedNumAtoms();
    for (int i = numMultipoles; i < paddedNumAtoms; i++) {
        dampingAndTholeVec.push_back(make_float2(0, 0));
        polarizabilityVec.push_back(0);
        multipoleParticlesVec.push_back(make_int4(0, 0, 0, 0));
        for (int j = 0; j < 3; j++)
            molecularDipolesVec.push_back(0);
        for (int j = 0; j < 5; j++)
            molecularQuadrupolesVec.push_back(0);
    }
    dampingAndThole = CudaArray::create<float2>(cu, paddedNumAtoms, "dampingAndThole");
    polarizability = CudaArray::create<float>(cu, paddedNumAtoms, "polarizability");
    multipoleParticles = CudaArray::create<int4>(cu, paddedNumAtoms, "multipoleParticles");
    molecularDipoles = CudaArray::create<float>(cu, 3*paddedNumAtoms, "molecularDipoles");
    molecularQuadrupoles = CudaArray::create<float>(cu, 5*paddedNumAtoms, "molecularQuadrupoles");
    lastPositions = new CudaArray(cu, cu.getPosq().getSize(), cu.getPosq().getElementSize(), "lastPositions");
    dampingAndThole->upload(dampingAndTholeVec);
    polarizability->upload(polarizabilityVec);
    multipoleParticles->upload(multipoleParticlesVec);
    molecularDipoles->upload(molecularDipolesVec);
    molecularQuadrupoles->upload(molecularQuadrupolesVec);
    posq.upload(&temp[0]);

    // Create workspace arrays.

    int elementSize = (cu.getUseDoublePrecision() ? sizeof(double) : sizeof(float));
    labFrameDipoles = new CudaArray(cu, 3*paddedNumAtoms, elementSize, "labFrameDipoles");
    labFrameQuadrupoles = new CudaArray(cu, 5*paddedNumAtoms, elementSize, "labFrameQuadrupoles");
    fracDipoles = new CudaArray(cu, 3*paddedNumAtoms, elementSize, "fracDipoles");
    fracQuadrupoles = new CudaArray(cu, 6*paddedNumAtoms, elementSize, "fracQuadrupoles");
    field = new CudaArray(cu, 3*paddedNumAtoms, sizeof(long long), "field");
    fieldPolar = new CudaArray(cu, 3*paddedNumAtoms, sizeof(long long), "fieldPolar");
    torque = new CudaArray(cu, 3*paddedNumAtoms, sizeof(long long), "torque");
    inducedDipole = new CudaArray(cu, 3*paddedNumAtoms, elementSize, "inducedDipole");
    inducedDipolePolar = new CudaArray(cu, 3*paddedNumAtoms, elementSize, "inducedDipolePolar");
    inducedDipoleErrors = new CudaArray(cu, cu.getNumThreadBlocks(), sizeof(float2), "inducedDipoleErrors");
    prevDipoles = new CudaArray(cu, 3*numMultipoles*MaxPrevDIISDipoles, elementSize, "prevDipoles");
    prevDipolesPolar = new CudaArray(cu, 3*numMultipoles*MaxPrevDIISDipoles, elementSize, "prevDipolesPolar");
    prevErrors = new CudaArray(cu, 3*numMultipoles*MaxPrevDIISDipoles, elementSize, "prevErrors");
    diisMatrix = new CudaArray(cu, MaxPrevDIISDipoles*MaxPrevDIISDipoles, elementSize, "diisMatrix");
    diisCoefficients = new CudaArray(cu, MaxPrevDIISDipoles+1, sizeof(float), "diisMatrix");
    cu.addAutoclearBuffer(*field);
    cu.addAutoclearBuffer(*fieldPolar);
    cu.addAutoclearBuffer(*torque);

    // Record which atoms should be flagged as exclusions based on covalent groups, and determine
    // the values for the covalent group flags.

    vector<vector<int> > exclusions(numMultipoles);
    for (int i = 0; i < numMultipoles; i++) {
        vector<int> atoms;
        set<int> allAtoms;
        allAtoms.insert(i);
        force.getCovalentMap(i, AmoebaMultipoleForce::Covalent12, atoms);
        allAtoms.insert(atoms.begin(), atoms.end());
        force.getCovalentMap(i, AmoebaMultipoleForce::Covalent13, atoms);
        allAtoms.insert(atoms.begin(), atoms.end());
        for (set<int>::const_iterator iter = allAtoms.begin(); iter != allAtoms.end(); ++iter)
            covalentFlagValues.push_back(make_int3(i, *iter, 0));
        force.getCovalentMap(i, AmoebaMultipoleForce::Covalent14, atoms);
        allAtoms.insert(atoms.begin(), atoms.end());
        for (int j = 0; j < (int) atoms.size(); j++)
            covalentFlagValues.push_back(make_int3(i, atoms[j], 1));
        force.getCovalentMap(i, AmoebaMultipoleForce::Covalent15, atoms);
        for (int j = 0; j < (int) atoms.size(); j++)
            covalentFlagValues.push_back(make_int3(i, atoms[j], 2));
        allAtoms.insert(atoms.begin(), atoms.end());
        force.getCovalentMap(i, AmoebaMultipoleForce::PolarizationCovalent11, atoms);
        allAtoms.insert(atoms.begin(), atoms.end());
        exclusions[i].insert(exclusions[i].end(), allAtoms.begin(), allAtoms.end());

        // Workaround for bug in TINKER: if an atom is listed in both the PolarizationCovalent11
        // and PolarizationCovalent12 maps, the latter takes precedence.

        vector<int> atoms12;
        force.getCovalentMap(i, AmoebaMultipoleForce::PolarizationCovalent12, atoms12);
        for (int j = 0; j < (int) atoms.size(); j++)
            if (find(atoms12.begin(), atoms12.end(), atoms[j]) == atoms12.end())
                polarizationFlagValues.push_back(make_int2(i, atoms[j]));
    }
    set<pair<int, int> > tilesWithExclusions;
    for (int atom1 = 0; atom1 < (int) exclusions.size(); ++atom1) {
        int x = atom1/CudaContext::TileSize;
        for (int j = 0; j < (int) exclusions[atom1].size(); ++j) {
            int atom2 = exclusions[atom1][j];
            int y = atom2/CudaContext::TileSize;
            tilesWithExclusions.insert(make_pair(max(x, y), min(x, y)));
        }
    }

    // Record other options.

    if (force.getPolarizationType() == AmoebaMultipoleForce::Mutual) {
        maxInducedIterations = force.getMutualInducedMaxIterations();
        inducedEpsilon = force.getMutualInducedTargetEpsilon();
        inducedField = new CudaArray(cu, 3*paddedNumAtoms, sizeof(long long), "inducedField");
        inducedFieldPolar = new CudaArray(cu, 3*paddedNumAtoms, sizeof(long long), "inducedFieldPolar");
    }
    else
        maxInducedIterations = 0;
    bool usePME = (force.getNonbondedMethod() == AmoebaMultipoleForce::PME);

    // Create the kernels.

    bool useShuffle = (cu.getComputeCapability() >= 3.0 && !cu.getUseDoublePrecision());
    double fixedThreadMemory = 19*elementSize+2*sizeof(float)+3*sizeof(int)/(double) cu.TileSize;
    double inducedThreadMemory = 15*elementSize+2*sizeof(float);
    double electrostaticsThreadMemory = 0;
    if (!useShuffle)
        fixedThreadMemory += 3*elementSize;
    map<string, string> defines;
    defines["NUM_ATOMS"] = cu.intToString(numMultipoles);
    defines["PADDED_NUM_ATOMS"] = cu.intToString(cu.getPaddedNumAtoms());
    defines["NUM_BLOCKS"] = cu.intToString(cu.getNumAtomBlocks());
    defines["ENERGY_SCALE_FACTOR"] = cu.doubleToString(138.9354558456/innerDielectric);
    if (force.getPolarizationType() == AmoebaMultipoleForce::Direct)
        defines["DIRECT_POLARIZATION"] = "";
    if (useShuffle)
        defines["USE_SHUFFLE"] = "";
    if (hasQuadrupoles)
        defines["INCLUDE_QUADRUPOLES"] = "";
    defines["TILE_SIZE"] = cu.intToString(CudaContext::TileSize);
    int numExclusionTiles = tilesWithExclusions.size();
    defines["NUM_TILES_WITH_EXCLUSIONS"] = cu.intToString(numExclusionTiles);
    int numContexts = cu.getPlatformData().contexts.size();
    int startExclusionIndex = cu.getContextIndex()*numExclusionTiles/numContexts;
    int endExclusionIndex = (cu.getContextIndex()+1)*numExclusionTiles/numContexts;
    defines["FIRST_EXCLUSION_TILE"] = cu.intToString(startExclusionIndex);
    defines["LAST_EXCLUSION_TILE"] = cu.intToString(endExclusionIndex);
    double alpha = force.getAEwald();
    int gridSizeX, gridSizeY, gridSizeZ;
    if (usePME) {
        vector<int> pmeGridDimension;
        force.getPmeGridDimensions(pmeGridDimension);
        if (pmeGridDimension[0] == 0 || alpha == 0.0) {
            NonbondedForce nb;
            nb.setEwaldErrorTolerance(force.getEwaldErrorTolerance());
            nb.setCutoffDistance(force.getCutoffDistance());
            NonbondedForceImpl::calcPMEParameters(system, nb, alpha, gridSizeX, gridSizeY, gridSizeZ);
            gridSizeX = findFFTDimension(gridSizeX);
            gridSizeY = findFFTDimension(gridSizeY);
            gridSizeZ = findFFTDimension(gridSizeZ);
        } else {
            gridSizeX = pmeGridDimension[0];
            gridSizeY = pmeGridDimension[1];
            gridSizeZ = pmeGridDimension[2];
        }
        defines["EWALD_ALPHA"] = cu.doubleToString(alpha);
        defines["SQRT_PI"] = cu.doubleToString(sqrt(M_PI));
        defines["USE_EWALD"] = "";
        defines["USE_CUTOFF"] = "";
        defines["USE_PERIODIC"] = "";
        defines["CUTOFF_SQUARED"] = cu.doubleToString(force.getCutoffDistance()*force.getCutoffDistance());
    }
    int maxThreads = cu.getNonbondedUtilities().getForceThreadBlockSize();
    fixedFieldThreads = min(maxThreads, cu.computeThreadBlockSize(fixedThreadMemory));
    inducedFieldThreads = min(maxThreads, cu.computeThreadBlockSize(inducedThreadMemory));
    CUmodule module = cu.createModule(CudaKernelSources::vectorOps+CudaAmoebaKernelSources::multipoles, defines);
    computeMomentsKernel = cu.getKernel(module, "computeLabFrameMoments");
    recordInducedDipolesKernel = cu.getKernel(module, "recordInducedDipoles");
    mapTorqueKernel = cu.getKernel(module, "mapTorqueToForce");
    computePotentialKernel = cu.getKernel(module, "computePotentialAtPoints");
    defines["THREAD_BLOCK_SIZE"] = cu.intToString(fixedFieldThreads);
    module = cu.createModule(CudaKernelSources::vectorOps+CudaAmoebaKernelSources::multipoleFixedField, defines);
    computeFixedFieldKernel = cu.getKernel(module, "computeFixedField");
    if (maxInducedIterations > 0) {
        defines["THREAD_BLOCK_SIZE"] = cu.intToString(inducedFieldThreads);
        defines["MAX_PREV_DIIS_DIPOLES"] = cu.intToString(MaxPrevDIISDipoles);
        module = cu.createModule(CudaKernelSources::vectorOps+CudaAmoebaKernelSources::multipoleInducedField, defines);
        computeInducedFieldKernel = cu.getKernel(module, "computeInducedField");
        updateInducedFieldKernel = cu.getKernel(module, "updateInducedFieldByDIIS");
        recordDIISDipolesKernel = cu.getKernel(module, "recordInducedDipolesForDIIS");
        buildMatrixKernel = cu.getKernel(module, "computeDIISMatrix");
    }
    stringstream electrostaticsSource;
    if (usePME) {
        electrostaticsSource << CudaKernelSources::vectorOps;
        electrostaticsSource << CudaAmoebaKernelSources::pmeMultipoleElectrostatics;
        electrostaticsSource << (hasQuadrupoles ? CudaAmoebaKernelSources::pmeElectrostaticPairForce : CudaAmoebaKernelSources::pmeElectrostaticPairForceNoQuadrupoles);
        electrostaticsSource << "#define APPLY_SCALE\n";
        electrostaticsSource << (hasQuadrupoles ? CudaAmoebaKernelSources::pmeElectrostaticPairForce : CudaAmoebaKernelSources::pmeElectrostaticPairForceNoQuadrupoles);
        electrostaticsThreadMemory = 24*elementSize+3*sizeof(float)+3*sizeof(int)/(double) cu.TileSize;
        if (!useShuffle)
            electrostaticsThreadMemory += 3*elementSize;
    }
    else {
        electrostaticsSource << CudaKernelSources::vectorOps;
        electrostaticsSource << CudaAmoebaKernelSources::multipoleElectrostatics;
        electrostaticsSource << "#define F1\n";
        electrostaticsSource << (hasQuadrupoles ? CudaAmoebaKernelSources::electrostaticPairForce : CudaAmoebaKernelSources::electrostaticPairForceNoQuadrupoles);
        electrostaticsSource << "#undef F1\n";
        electrostaticsSource << "#define T1\n";
        electrostaticsSource << (hasQuadrupoles ? CudaAmoebaKernelSources::electrostaticPairForce : CudaAmoebaKernelSources::electrostaticPairForceNoQuadrupoles);
        electrostaticsSource << "#undef T1\n";
        electrostaticsSource << "#define T3\n";
        electrostaticsSource << (hasQuadrupoles ? CudaAmoebaKernelSources::electrostaticPairForce : CudaAmoebaKernelSources::electrostaticPairForceNoQuadrupoles);
        electrostaticsThreadMemory = 21*elementSize+2*sizeof(float)+3*sizeof(int)/(double) cu.TileSize;
        if (!useShuffle)
            electrostaticsThreadMemory += 3*elementSize;
    }
    electrostaticsThreads = min(maxThreads, cu.computeThreadBlockSize(electrostaticsThreadMemory));
    defines["THREAD_BLOCK_SIZE"] = cu.intToString(electrostaticsThreads);
    module = cu.createModule(electrostaticsSource.str(), defines);
    electrostaticsKernel = cu.getKernel(module, "computeElectrostatics");

    // Set up PME.

    if (usePME) {
        // Create the PME kernels.

        map<string, string> pmeDefines;
        pmeDefines["EWALD_ALPHA"] = cu.doubleToString(alpha);
        pmeDefines["PME_ORDER"] = cu.intToString(PmeOrder);
        pmeDefines["NUM_ATOMS"] = cu.intToString(numMultipoles);
        pmeDefines["PADDED_NUM_ATOMS"] = cu.intToString(cu.getPaddedNumAtoms());
        pmeDefines["EPSILON_FACTOR"] = cu.doubleToString(138.9354558456);
        pmeDefines["GRID_SIZE_X"] = cu.intToString(gridSizeX);
        pmeDefines["GRID_SIZE_Y"] = cu.intToString(gridSizeY);
        pmeDefines["GRID_SIZE_Z"] = cu.intToString(gridSizeZ);
        pmeDefines["M_PI"] = cu.doubleToString(M_PI);
        pmeDefines["SQRT_PI"] = cu.doubleToString(sqrt(M_PI));
        if (force.getPolarizationType() == AmoebaMultipoleForce::Direct)
            pmeDefines["DIRECT_POLARIZATION"] = "";
        CUmodule module = cu.createModule(CudaKernelSources::vectorOps+CudaAmoebaKernelSources::multipolePme, pmeDefines);
        pmeGridIndexKernel = cu.getKernel(module, "findAtomGridIndex");
        pmeTransformMultipolesKernel = cu.getKernel(module, "transformMultipolesToFractionalCoordinates");
        pmeTransformPotentialKernel = cu.getKernel(module, "transformPotentialToCartesianCoordinates");
        pmeSpreadFixedMultipolesKernel = cu.getKernel(module, "gridSpreadFixedMultipoles");
        pmeSpreadInducedDipolesKernel = cu.getKernel(module, "gridSpreadInducedDipoles");
        pmeFinishSpreadChargeKernel = cu.getKernel(module, "finishSpreadCharge");
        pmeConvolutionKernel = cu.getKernel(module, "reciprocalConvolution");
        pmeFixedPotentialKernel = cu.getKernel(module, "computeFixedPotentialFromGrid");
        pmeInducedPotentialKernel = cu.getKernel(module, "computeInducedPotentialFromGrid");
        pmeFixedForceKernel = cu.getKernel(module, "computeFixedMultipoleForceAndEnergy");
        pmeInducedForceKernel = cu.getKernel(module, "computeInducedDipoleForceAndEnergy");
        pmeRecordInducedFieldDipolesKernel = cu.getKernel(module, "recordInducedFieldDipoles");
        cuFuncSetCacheConfig(pmeSpreadFixedMultipolesKernel, CU_FUNC_CACHE_PREFER_L1);
        cuFuncSetCacheConfig(pmeSpreadInducedDipolesKernel, CU_FUNC_CACHE_PREFER_L1);
        cuFuncSetCacheConfig(pmeFixedPotentialKernel, CU_FUNC_CACHE_PREFER_L1);
        cuFuncSetCacheConfig(pmeInducedPotentialKernel, CU_FUNC_CACHE_PREFER_L1);

        // Create required data structures.

        int elementSize = (cu.getUseDoublePrecision() ? sizeof(double) : sizeof(float));
        pmeGrid = new CudaArray(cu, gridSizeX*gridSizeY*gridSizeZ, 2*elementSize, "pmeGrid");
        cu.addAutoclearBuffer(*pmeGrid);
        pmeBsplineModuliX = new CudaArray(cu, gridSizeX, elementSize, "pmeBsplineModuliX");
        pmeBsplineModuliY = new CudaArray(cu, gridSizeY, elementSize, "pmeBsplineModuliY");
        pmeBsplineModuliZ = new CudaArray(cu, gridSizeZ, elementSize, "pmeBsplineModuliZ");
        pmeIgrid = CudaArray::create<int4>(cu, numMultipoles, "pmeIgrid");
        pmePhi = new CudaArray(cu, 20*numMultipoles, elementSize, "pmePhi");
        pmePhid = new CudaArray(cu, 10*numMultipoles, elementSize, "pmePhid");
        pmePhip = new CudaArray(cu, 10*numMultipoles, elementSize, "pmePhip");
        pmePhidp = new CudaArray(cu, 20*numMultipoles, elementSize, "pmePhidp");
        pmeCphi = new CudaArray(cu, 10*numMultipoles, elementSize, "pmeCphi");
        pmeAtomRange = CudaArray::create<int>(cu, gridSizeX*gridSizeY*gridSizeZ+1, "pmeAtomRange");
        pmeAtomGridIndex = CudaArray::create<int2>(cu, numMultipoles, "pmeAtomGridIndex");
        sort = new CudaSort(cu, new SortTrait(), cu.getNumAtoms());
        cufftResult result = cufftPlan3d(&fft, gridSizeX, gridSizeY, gridSizeZ, cu.getUseDoublePrecision() ? CUFFT_Z2Z : CUFFT_C2C);
        if (result != CUFFT_SUCCESS)
            throw OpenMMException("Error initializing FFT: "+cu.intToString(result));
        hasInitializedFFT = true;

        // Initialize the b-spline moduli.

        double data[PmeOrder];
        double x = 0.0;
        data[0] = 1.0 - x;
        data[1] = x;
        for (int i = 2; i < PmeOrder; i++) {
            double denom = 1.0/i;
            data[i] = x*data[i-1]*denom;
            for (int j = 1; j < i; j++)
                data[i-j] = ((x+j)*data[i-j-1] + ((i-j+1)-x)*data[i-j])*denom;
            data[0] = (1.0-x)*data[0]*denom;
        }
        int maxSize = max(max(gridSizeX, gridSizeY), gridSizeZ);
        vector<double> bsplines_data(maxSize+1, 0.0);
        for (int i = 2; i <= PmeOrder+1; i++)
            bsplines_data[i] = data[i-2];
        for (int dim = 0; dim < 3; dim++) {
            int ndata = (dim == 0 ? gridSizeX : dim == 1 ? gridSizeY : gridSizeZ);
            vector<double> moduli(ndata);

            // get the modulus of the discrete Fourier transform

            double factor = 2.0*M_PI/ndata;
            for (int i = 0; i < ndata; i++) {
                double sc = 0.0;
                double ss = 0.0;
                for (int j = 1; j <= ndata; j++) {
                    double arg = factor*i*(j-1);
                    sc += bsplines_data[j]*cos(arg);
                    ss += bsplines_data[j]*sin(arg);
                }
                moduli[i] = sc*sc+ss*ss;
            }

            // Fix for exponential Euler spline interpolation failure.

            double eps = 1.0e-7;
            if (moduli[0] < eps)
                moduli[0] = 0.9*moduli[1];
            for (int i = 1; i < ndata-1; i++)
                if (moduli[i] < eps)
                    moduli[i] = 0.9*(moduli[i-1]+moduli[i+1]);
            if (moduli[ndata-1] < eps)
                moduli[ndata-1] = 0.9*moduli[ndata-2];

            // Compute and apply the optimal zeta coefficient.

            int jcut = 50;
            for (int i = 1; i <= ndata; i++) {
                int k = i - 1;
                if (i > ndata/2)
                    k = k - ndata;
                double zeta;
                if (k == 0)
                    zeta = 1.0;
                else {
                    double sum1 = 1.0;
                    double sum2 = 1.0;
                    factor = M_PI*k/ndata;
                    for (int j = 1; j <= jcut; j++) {
                        double arg = factor/(factor+M_PI*j);
                        sum1 += pow(arg, PmeOrder);
                        sum2 += pow(arg, 2*PmeOrder);
                    }
                    for (int j = 1; j <= jcut; j++) {
                        double arg = factor/(factor-M_PI*j);
                        sum1 += pow(arg, PmeOrder);
                        sum2 += pow(arg, 2*PmeOrder);
                    }
                    zeta = sum2/sum1;
                }
                moduli[i-1] = moduli[i-1]*zeta*zeta;
            }
            if (cu.getUseDoublePrecision()) {
                if (dim == 0)
                    pmeBsplineModuliX->upload(moduli);
                else if (dim == 1)
                    pmeBsplineModuliY->upload(moduli);
                else
                    pmeBsplineModuliZ->upload(moduli);
            }
            else {
                vector<float> modulif(ndata);
                for (int i = 0; i < ndata; i++)
                    modulif[i] = (float) moduli[i];
                if (dim == 0)
                    pmeBsplineModuliX->upload(modulif);
                else if (dim == 1)
                    pmeBsplineModuliY->upload(modulif);
                else
                    pmeBsplineModuliZ->upload(modulif);
            }
        }
    }

    // Add an interaction to the default nonbonded kernel.  This doesn't actually do any calculations.  It's
    // just so that CudaNonbondedUtilities will build the exclusion flags and maintain the neighbor list.

    cu.getNonbondedUtilities().addInteraction(usePME, usePME, true, force.getCutoffDistance(), exclusions, "", force.getForceGroup());
    cu.getNonbondedUtilities().setUsePadding(false);
    cu.addForce(new ForceInfo(force));
}

void CudaCalcMBPolElectrostaticsForceKernel::initializeScaleFactors() {
    hasInitializedScaleFactors = true;
    CudaNonbondedUtilities& nb = cu.getNonbondedUtilities();

    // Figure out the covalent flag values to use for each atom pair.

    vector<ushort2> exclusionTiles;
    nb.getExclusionTiles().download(exclusionTiles);
    map<pair<int, int>, int> exclusionTileMap;
    for (int i = 0; i < (int) exclusionTiles.size(); i++) {
        ushort2 tile = exclusionTiles[i];
        exclusionTileMap[make_pair(tile.x, tile.y)] = i;
    }
    covalentFlags = CudaArray::create<uint2>(cu, nb.getExclusions().getSize(), "covalentFlags");
    vector<uint2> covalentFlagsVec(nb.getExclusions().getSize(), make_uint2(0, 0));
    for (int i = 0; i < (int) covalentFlagValues.size(); i++) {
        int atom1 = covalentFlagValues[i].x;
        int atom2 = covalentFlagValues[i].y;
        int value = covalentFlagValues[i].z;
        int x = atom1/CudaContext::TileSize;
        int offset1 = atom1-x*CudaContext::TileSize;
        int y = atom2/CudaContext::TileSize;
        int offset2 = atom2-y*CudaContext::TileSize;
        int f1 = (value == 0 || value == 1 ? 1 : 0);
        int f2 = (value == 0 || value == 2 ? 1 : 0);
        if (x == y) {
            int index = exclusionTileMap[make_pair(x, y)]*CudaContext::TileSize;
            covalentFlagsVec[index+offset1].x |= f1<<offset2;
            covalentFlagsVec[index+offset1].y |= f2<<offset2;
            covalentFlagsVec[index+offset2].x |= f1<<offset1;
            covalentFlagsVec[index+offset2].y |= f2<<offset1;
        }
        else if (x > y) {
            int index = exclusionTileMap[make_pair(x, y)]*CudaContext::TileSize;
            covalentFlagsVec[index+offset1].x |= f1<<offset2;
            covalentFlagsVec[index+offset1].y |= f2<<offset2;
        }
        else {
            int index = exclusionTileMap[make_pair(y, x)]*CudaContext::TileSize;
            covalentFlagsVec[index+offset2].x |= f1<<offset1;
            covalentFlagsVec[index+offset2].y |= f2<<offset1;
        }
    }
    covalentFlags->upload(covalentFlagsVec);

    // Do the same for the polarization flags.

    polarizationGroupFlags = CudaArray::create<unsigned int>(cu, nb.getExclusions().getSize(), "polarizationGroupFlags");
    vector<unsigned int> polarizationGroupFlagsVec(nb.getExclusions().getSize(), 0);
    for (int i = 0; i < (int) polarizationFlagValues.size(); i++) {
        int atom1 = polarizationFlagValues[i].x;
        int atom2 = polarizationFlagValues[i].y;
        int x = atom1/CudaContext::TileSize;
        int offset1 = atom1-x*CudaContext::TileSize;
        int y = atom2/CudaContext::TileSize;
        int offset2 = atom2-y*CudaContext::TileSize;
        if (x == y) {
            int index = exclusionTileMap[make_pair(x, y)]*CudaContext::TileSize;
            polarizationGroupFlagsVec[index+offset1] |= 1<<offset2;
            polarizationGroupFlagsVec[index+offset2] |= 1<<offset1;
        }
        else if (x > y) {
            int index = exclusionTileMap[make_pair(x, y)]*CudaContext::TileSize;
            polarizationGroupFlagsVec[index+offset1] |= 1<<offset2;
        }
        else {
            int index = exclusionTileMap[make_pair(y, x)]*CudaContext::TileSize;
            polarizationGroupFlagsVec[index+offset2] |= 1<<offset1;
        }
    }
    polarizationGroupFlags->upload(polarizationGroupFlagsVec);
}

double CudaCalcMBPolElectrostaticsForceKernel::execute(ContextImpl& context, bool includeForces, bool includeEnergy) {
    if (!hasInitializedScaleFactors) {
        initializeScaleFactors();
    }
    CudaNonbondedUtilities& nb = cu.getNonbondedUtilities();

    // Compute the lab frame moments.

    void* computeMomentsArgs[] = {&cu.getPosq().getDevicePointer(), &multipoleParticles->getDevicePointer(),
        &molecularDipoles->getDevicePointer(), &molecularQuadrupoles->getDevicePointer(),
        &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer()};
    cu.executeKernel(computeMomentsKernel, computeMomentsArgs, cu.getNumAtoms());
    int startTileIndex = nb.getStartTileIndex();
    int numTileIndices = nb.getNumTiles();
    int numForceThreadBlocks = nb.getNumForceThreadBlocks();
    int elementSize = (cu.getUseDoublePrecision() ? sizeof(double) : sizeof(float));
    void* npt = NULL;
    if (pmeGrid == NULL) {
        // Compute induced dipoles.

            void* computeFixedFieldArgs[] = {&field->getDevicePointer(), &fieldPolar->getDevicePointer(), &cu.getPosq().getDevicePointer(),
                &covalentFlags->getDevicePointer(), &polarizationGroupFlags->getDevicePointer(), &nb.getExclusionTiles().getDevicePointer(), &startTileIndex, &numTileIndices,
                &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(), &dampingAndThole->getDevicePointer()};
            cu.executeKernel(computeFixedFieldKernel, computeFixedFieldArgs, numForceThreadBlocks*fixedFieldThreads, fixedFieldThreads);
            void* recordInducedDipolesArgs[] = {&field->getDevicePointer(), &fieldPolar->getDevicePointer(),
                &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(), &polarizability->getDevicePointer()};
            cu.executeKernel(recordInducedDipolesKernel, recordInducedDipolesArgs, cu.getNumAtoms());


        // Iterate until the dipoles converge.

        for (int i = 0; i < maxInducedIterations; i++) {
            cu.clearBuffer(*inducedField);
            cu.clearBuffer(*inducedFieldPolar);

                void* computeInducedFieldArgs[] = {&inducedField->getDevicePointer(), &inducedFieldPolar->getDevicePointer(), &cu.getPosq().getDevicePointer(),
                    &nb.getExclusionTiles().getDevicePointer(), &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(), &startTileIndex, &numTileIndices,
                    &dampingAndThole->getDevicePointer()};
                cu.executeKernel(computeInducedFieldKernel, computeInducedFieldArgs, numForceThreadBlocks*inducedFieldThreads, inducedFieldThreads);

            bool converged = iterateDipolesByDIIS(i);
            if (converged)
                break;
        }

        // Compute electrostatic force.

        void* electrostaticsArgs[] = {&cu.getForce().getDevicePointer(), &torque->getDevicePointer(), &cu.getEnergyBuffer().getDevicePointer(),
            &cu.getPosq().getDevicePointer(), &covalentFlags->getDevicePointer(), &polarizationGroupFlags->getDevicePointer(),
            &nb.getExclusionTiles().getDevicePointer(), &startTileIndex, &numTileIndices,
            &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(), &inducedDipole->getDevicePointer(),
            &inducedDipolePolar->getDevicePointer(), &dampingAndThole->getDevicePointer()};
        cu.executeKernel(electrostaticsKernel, electrostaticsArgs, numForceThreadBlocks*electrostaticsThreads, electrostaticsThreads);
    }
    else {
        // Compute reciprocal box vectors.

        Vec3 boxVectors[3];
        cu.getPeriodicBoxVectors(boxVectors[0], boxVectors[1], boxVectors[2]);
        double determinant = boxVectors[0][0]*boxVectors[1][1]*boxVectors[2][2];
        double scale = 1.0/determinant;
        double3 recipBoxVectors[3];
        recipBoxVectors[0] = make_double3(boxVectors[1][1]*boxVectors[2][2]*scale, 0, 0);
        recipBoxVectors[1] = make_double3(-boxVectors[1][0]*boxVectors[2][2]*scale, boxVectors[0][0]*boxVectors[2][2]*scale, 0);
        recipBoxVectors[2] = make_double3((boxVectors[1][0]*boxVectors[2][1]-boxVectors[1][1]*boxVectors[2][0])*scale, -boxVectors[0][0]*boxVectors[2][1]*scale, boxVectors[0][0]*boxVectors[1][1]*scale);
        float3 recipBoxVectorsFloat[3];
        void* recipBoxVectorPointer[3];
        if (cu.getUseDoublePrecision()) {
            recipBoxVectorPointer[0] = &recipBoxVectors[0];
            recipBoxVectorPointer[1] = &recipBoxVectors[1];
            recipBoxVectorPointer[2] = &recipBoxVectors[2];
        }
        else {
            recipBoxVectorsFloat[0] = make_float3((float) recipBoxVectors[0].x, 0, 0);
            recipBoxVectorsFloat[1] = make_float3((float) recipBoxVectors[1].x, (float) recipBoxVectors[1].y, 0);
            recipBoxVectorsFloat[2] = make_float3((float) recipBoxVectors[2].x, (float) recipBoxVectors[2].y, (float) recipBoxVectors[2].z);
            recipBoxVectorPointer[0] = &recipBoxVectorsFloat[0];
            recipBoxVectorPointer[1] = &recipBoxVectorsFloat[1];
            recipBoxVectorPointer[2] = &recipBoxVectorsFloat[2];
        }

        // Reciprocal space calculation.

        unsigned int maxTiles = nb.getInteractingTiles().getSize();
        void* gridIndexArgs[] = {&cu.getPosq().getDevicePointer(), &pmeAtomGridIndex->getDevicePointer(),
            cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeGridIndexKernel, gridIndexArgs, cu.getNumAtoms(), cu.ThreadBlockSize, cu.ThreadBlockSize*PmeOrder*PmeOrder*elementSize);
        sort->sort(*pmeAtomGridIndex);
        void* pmeTransformMultipolesArgs[] = {&labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(),
            &fracDipoles->getDevicePointer(), &fracQuadrupoles->getDevicePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeTransformMultipolesKernel, pmeTransformMultipolesArgs, cu.getNumAtoms());
        void* pmeSpreadFixedMultipolesArgs[] = {&cu.getPosq().getDevicePointer(), &fracDipoles->getDevicePointer(), &fracQuadrupoles->getDevicePointer(),
            &pmeGrid->getDevicePointer(), &pmeAtomGridIndex->getDevicePointer(),  cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeSpreadFixedMultipolesKernel, pmeSpreadFixedMultipolesArgs, cu.getNumAtoms());
        void* finishSpreadArgs[] = {&pmeGrid->getDevicePointer()};
        if (cu.getUseDoublePrecision())
            cu.executeKernel(pmeFinishSpreadChargeKernel, finishSpreadArgs, pmeGrid->getSize());
        if (cu.getUseDoublePrecision())
            cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
        else
            cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
        void* pmeConvolutionArgs[] = {&pmeGrid->getDevicePointer(), &pmeBsplineModuliX->getDevicePointer(), &pmeBsplineModuliY->getDevicePointer(),
            &pmeBsplineModuliZ->getDevicePointer(), cu.getPeriodicBoxSizePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeConvolutionKernel, pmeConvolutionArgs, cu.getNumAtoms());
        if (cu.getUseDoublePrecision())
            cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
        else
            cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
        void* pmeFixedPotentialArgs[] = {&pmeGrid->getDevicePointer(), &pmePhi->getDevicePointer(), &field->getDevicePointer(),
            &fieldPolar ->getDevicePointer(), &cu.getPosq().getDevicePointer(), &labFrameDipoles->getDevicePointer(),
            cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2], &pmeAtomGridIndex->getDevicePointer()};
        cu.executeKernel(pmeFixedPotentialKernel, pmeFixedPotentialArgs, cu.getNumAtoms());
        void* pmeTransformFixedPotentialArgs[] = {&pmePhi->getDevicePointer(), &pmeCphi->getDevicePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeTransformPotentialKernel, pmeTransformFixedPotentialArgs, cu.getNumAtoms());
        void* pmeFixedForceArgs[] = {&cu.getPosq().getDevicePointer(), &cu.getForce().getDevicePointer(), &torque->getDevicePointer(),
            &cu.getEnergyBuffer().getDevicePointer(), &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(),
            &fracDipoles->getDevicePointer(), &fracQuadrupoles->getDevicePointer(), &pmePhi->getDevicePointer(), &pmeCphi->getDevicePointer(),
            recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeFixedForceKernel, pmeFixedForceArgs, cu.getNumAtoms());

        // Direct space calculation.

        void* computeFixedFieldArgs[] = {&field->getDevicePointer(), &fieldPolar->getDevicePointer(), &cu.getPosq().getDevicePointer(),
            &covalentFlags->getDevicePointer(), &polarizationGroupFlags->getDevicePointer(), &nb.getExclusionTiles().getDevicePointer(), &startTileIndex, &numTileIndices,
            &nb.getInteractingTiles().getDevicePointer(), &nb.getInteractionCount().getDevicePointer(), cu.getPeriodicBoxSizePointer(),
            cu.getInvPeriodicBoxSizePointer(), cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            &maxTiles, &nb.getBlockCenters().getDevicePointer(), &nb.getInteractingAtoms().getDevicePointer(),
            &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(), &dampingAndThole->getDevicePointer()};
        cu.executeKernel(computeFixedFieldKernel, computeFixedFieldArgs, numForceThreadBlocks*fixedFieldThreads, fixedFieldThreads);
        void* recordInducedDipolesArgs[] = {&field->getDevicePointer(), &fieldPolar->getDevicePointer(),
            &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(), &polarizability->getDevicePointer()};
        cu.executeKernel(recordInducedDipolesKernel, recordInducedDipolesArgs, cu.getNumAtoms());

        // Reciprocal space calculation for the induced dipoles.

        cu.clearBuffer(*pmeGrid);
        void* pmeSpreadInducedDipolesArgs[] = {&cu.getPosq().getDevicePointer(), &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(),
            &pmeGrid->getDevicePointer(), &pmeAtomGridIndex->getDevicePointer(), cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeSpreadInducedDipolesKernel, pmeSpreadInducedDipolesArgs, cu.getNumAtoms());
        if (cu.getUseDoublePrecision())
            cu.executeKernel(pmeFinishSpreadChargeKernel, finishSpreadArgs, pmeGrid->getSize());
        if (cu.getUseDoublePrecision())
            cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
        else
            cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
        cu.executeKernel(pmeConvolutionKernel, pmeConvolutionArgs, cu.getNumAtoms());
        if (cu.getUseDoublePrecision())
            cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
        else
            cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
        void* pmeInducedPotentialArgs[] = {&pmeGrid->getDevicePointer(), &pmePhid->getDevicePointer(), &pmePhip->getDevicePointer(),
            &pmePhidp->getDevicePointer(), &cu.getPosq().getDevicePointer(), cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(),
            cu.getPeriodicBoxVecZPointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2],
            &pmeAtomGridIndex->getDevicePointer()};
        cu.executeKernel(pmeInducedPotentialKernel, pmeInducedPotentialArgs, cu.getNumAtoms());

        // Iterate until the dipoles converge.

        vector<float2> errors;
        for (int i = 0; i < maxInducedIterations; i++) {
            cu.clearBuffer(*inducedField);
            cu.clearBuffer(*inducedFieldPolar);
            void* computeInducedFieldArgs[] = {&inducedField->getDevicePointer(), &inducedFieldPolar->getDevicePointer(), &cu.getPosq().getDevicePointer(),
                &nb.getExclusionTiles().getDevicePointer(), &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(), &startTileIndex, &numTileIndices,
                &nb.getInteractingTiles().getDevicePointer(), &nb.getInteractionCount().getDevicePointer(), cu.getPeriodicBoxSizePointer(),
                cu.getInvPeriodicBoxSizePointer(), cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
                &maxTiles, &nb.getBlockCenters().getDevicePointer(), &nb.getInteractingAtoms().getDevicePointer(),
                &dampingAndThole->getDevicePointer()};
            cu.executeKernel(computeInducedFieldKernel, computeInducedFieldArgs, numForceThreadBlocks*inducedFieldThreads, inducedFieldThreads);
            cu.clearBuffer(*pmeGrid);
            cu.executeKernel(pmeSpreadInducedDipolesKernel, pmeSpreadInducedDipolesArgs, cu.getNumAtoms());
            if (cu.getUseDoublePrecision())
                cu.executeKernel(pmeFinishSpreadChargeKernel, finishSpreadArgs, pmeGrid->getSize());
            if (cu.getUseDoublePrecision())
                cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
            else
                cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_FORWARD);
            cu.executeKernel(pmeConvolutionKernel, pmeConvolutionArgs, cu.getNumAtoms());
            if (cu.getUseDoublePrecision())
                cufftExecZ2Z(fft, (double2*) pmeGrid->getDevicePointer(), (double2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
            else
                cufftExecC2C(fft, (float2*) pmeGrid->getDevicePointer(), (float2*) pmeGrid->getDevicePointer(), CUFFT_INVERSE);
            cu.executeKernel(pmeInducedPotentialKernel, pmeInducedPotentialArgs, cu.getNumAtoms());
            void* pmeRecordInducedFieldDipolesArgs[] = {&pmePhid->getDevicePointer(), &pmePhip->getDevicePointer(),
                &inducedField->getDevicePointer(), &inducedFieldPolar->getDevicePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
            cu.executeKernel(pmeRecordInducedFieldDipolesKernel, pmeRecordInducedFieldDipolesArgs, cu.getNumAtoms());
            bool converged = iterateDipolesByDIIS(i);
            if (converged)
                break;
        }

        // Compute electrostatic force.

        void* electrostaticsArgs[] = {&cu.getForce().getDevicePointer(), &torque->getDevicePointer(), &cu.getEnergyBuffer().getDevicePointer(),
            &cu.getPosq().getDevicePointer(), &covalentFlags->getDevicePointer(), &polarizationGroupFlags->getDevicePointer(),
            &nb.getExclusionTiles().getDevicePointer(), &startTileIndex, &numTileIndices,
            &nb.getInteractingTiles().getDevicePointer(), &nb.getInteractionCount().getDevicePointer(),
            cu.getPeriodicBoxSizePointer(), cu.getInvPeriodicBoxSizePointer(), cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer(),
            &maxTiles, &nb.getBlockCenters().getDevicePointer(), &nb.getInteractingAtoms().getDevicePointer(),
            &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(), &inducedDipole->getDevicePointer(),
            &inducedDipolePolar->getDevicePointer(), &dampingAndThole->getDevicePointer()};
        cu.executeKernel(electrostaticsKernel, electrostaticsArgs, numForceThreadBlocks*electrostaticsThreads, electrostaticsThreads);
        void* pmeTransformInducedPotentialArgs[] = {&pmePhidp->getDevicePointer(), &pmeCphi->getDevicePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeTransformPotentialKernel, pmeTransformInducedPotentialArgs, cu.getNumAtoms());
        void* pmeInducedForceArgs[] = {&cu.getPosq().getDevicePointer(), &cu.getForce().getDevicePointer(), &torque->getDevicePointer(),
            &cu.getEnergyBuffer().getDevicePointer(), &labFrameDipoles->getDevicePointer(), &labFrameQuadrupoles->getDevicePointer(),
            &fracDipoles->getDevicePointer(), &fracQuadrupoles->getDevicePointer(),
            &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(), &pmePhi->getDevicePointer(), &pmePhid->getDevicePointer(),
            &pmePhip->getDevicePointer(), &pmePhidp->getDevicePointer(), &pmeCphi->getDevicePointer(), recipBoxVectorPointer[0], recipBoxVectorPointer[1], recipBoxVectorPointer[2]};
        cu.executeKernel(pmeInducedForceKernel, pmeInducedForceArgs, cu.getNumAtoms());
    }

    // Map torques to force.

    void* mapTorqueArgs[] = {&cu.getForce().getDevicePointer(), &torque->getDevicePointer(),
        &cu.getPosq().getDevicePointer(), &multipoleParticles->getDevicePointer()};
    cu.executeKernel(mapTorqueKernel, mapTorqueArgs, cu.getNumAtoms());

    // Record the current atom positions so we can tell later if they have changed.

    cu.getPosq().copyTo(*lastPositions);
    multipolesAreValid = true;
    return 0.0;
}

bool CudaCalcMBPolElectrostaticsForceKernel::iterateDipolesByDIIS(int iteration) {
    void* npt = NULL;
    bool trueValue = true, falseValue = false;
    int elementSize = (cu.getUseDoublePrecision() ? sizeof(double) : sizeof(float));

    // Record the dipoles and errors into the lists of previous dipoles.

    void* recordDIISDipolesArgs[] = {&field->getDevicePointer(), &fieldPolar->getDevicePointer(), &npt, &inducedField->getDevicePointer(),
        &inducedFieldPolar->getDevicePointer(), &inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(),
        &polarizability->getDevicePointer(), &inducedDipoleErrors->getDevicePointer(), &prevDipoles->getDevicePointer(),
        &prevDipolesPolar->getDevicePointer(), &prevErrors->getDevicePointer(), &iteration, &trueValue, &diisMatrix->getDevicePointer()};
    cu.executeKernel(recordDIISDipolesKernel, recordDIISDipolesArgs, cu.getNumThreadBlocks()*cu.ThreadBlockSize, cu.ThreadBlockSize, cu.ThreadBlockSize*elementSize*2);
    float2* errors = (float2*) cu.getPinnedBuffer();
    inducedDipoleErrors->download(errors, false);

    // Build the DIIS matrix.

    int numPrev = (iteration+1 < MaxPrevDIISDipoles ? iteration+1 : MaxPrevDIISDipoles);
    void* buildMatrixArgs[] = {&prevErrors->getDevicePointer(), &iteration, &diisMatrix->getDevicePointer()};
    int threadBlocks = min(numPrev, cu.getNumThreadBlocks());
    cu.executeKernel(buildMatrixKernel, buildMatrixArgs, threadBlocks*128, 128, 128*elementSize);
    vector<float> matrixf;
    vector<double> matrix;
    if (cu.getUseDoublePrecision())
        diisMatrix->download(matrix);
    else
        diisMatrix->download(matrixf);

    // Determine whether the iteration has converged.

    double total1 = 0.0, total2 = 0.0;
    for (int j = 0; j < inducedDipoleErrors->getSize(); j++) {
        total1 += errors[j].x;
        total2 += errors[j].y;
    }
    if (48.033324*sqrt(max(total1, total2)/cu.getNumAtoms()) < inducedEpsilon)
        return true;

    // Compute the coefficients for selecting the new dipoles.

    float* coefficients = (float*) cu.getPinnedBuffer();
    if (iteration == 0)
        coefficients[0] = 1;
    else {
        int rank = numPrev+1;
        Array2D<double> b(rank, rank);
        b[0][0] = 0;
        for (int i = 1; i < rank; i++)
            b[i][0] = b[0][i] = -1;
        if (cu.getUseDoublePrecision()) {
            for (int i = 0; i < numPrev; i++)
                for (int j = 0; j < numPrev; j++)
                    b[i+1][j+1] = matrix[i*MaxPrevDIISDipoles+j];
        }
        else {
            for (int i = 0; i < numPrev; i++)
                for (int j = 0; j < numPrev; j++)
                    b[i+1][j+1] = matrixf[i*MaxPrevDIISDipoles+j];
        }

        // Solve using SVD.  Since the right hand side is (-1, 0, 0, 0, ...), this is simpler than the general case.

        JAMA::SVD<double> svd(b);
        Array2D<double> u, v;
        svd.getU(u);
        svd.getV(v);
        Array1D<double> s;
        svd.getSingularValues(s);
        int effectiveRank = svd.rank();
        for (int i = 1; i < rank; i++) {
            double d = 0;
            for (int j = 0; j < effectiveRank; j++)
                d -= u[0][j]*v[i][j]/s[j];
            coefficients[i-1] = d;
        }
    }
    diisCoefficients->upload(coefficients, false);

    // Compute the dipoles.

    void* updateInducedFieldArgs[] = {&inducedDipole->getDevicePointer(), &inducedDipolePolar->getDevicePointer(),
        &prevDipoles->getDevicePointer(), &prevDipolesPolar->getDevicePointer(), &diisCoefficients->getDevicePointer(), &numPrev};
    cu.executeKernel(updateInducedFieldKernel, updateInducedFieldArgs, cu.getNumThreadBlocks()*cu.ThreadBlockSize);
    return false;
}

void CudaCalcMBPolElectrostaticsForceKernel::ensureMultipolesValid(ContextImpl& context) {
    if (multipolesAreValid) {
        int numParticles = cu.getNumAtoms();
        if (cu.getUseDoublePrecision()) {
            vector<double4> pos1, pos2;
            cu.getPosq().download(pos1);
            lastPositions->download(pos2);
            for (int i = 0; i < numParticles; i++)
                if (pos1[i].x != pos2[i].x || pos1[i].y != pos2[i].y || pos1[i].z != pos2[i].z) {
                    multipolesAreValid = false;
                    break;
                }
        }
        else {
            vector<float4> pos1, pos2;
            cu.getPosq().download(pos1);
            lastPositions->download(pos2);
            for (int i = 0; i < numParticles; i++)
                if (pos1[i].x != pos2[i].x || pos1[i].y != pos2[i].y || pos1[i].z != pos2[i].z) {
                    multipolesAreValid = false;
                    break;
                }
        }
    }
    if (!multipolesAreValid)
        context.calcForcesAndEnergy(false, false, -1);
}

void CudaCalcMBPolElectrostaticsForceKernel::getInducedDipoles(ContextImpl& context, vector<Vec3>& dipoles) {
    ensureMultipolesValid(context);
    int numParticles = cu.getNumAtoms();
    dipoles.resize(numParticles);
    const vector<int>& order = cu.getAtomIndex();
    if (cu.getUseDoublePrecision()) {
        vector<double> d;
        inducedDipole->download(d);
        for (int i = 0; i < numParticles; i++)
            dipoles[order[i]] = Vec3(d[3*i], d[3*i+1], d[3*i+2]);
    }
    else {
        vector<float> d;
        inducedDipole->download(d);
        for (int i = 0; i < numParticles; i++)
            dipoles[order[i]] = Vec3(d[3*i], d[3*i+1], d[3*i+2]);
    }
}

void CudaCalcMBPolElectrostaticsForceKernel::getElectrostaticPotential(ContextImpl& context, const vector<Vec3>& inputGrid, vector<double>& outputElectrostaticPotential) {
    ensureMultipolesValid(context);
    int numPoints = inputGrid.size();
    int elementSize = (cu.getUseDoublePrecision() ? sizeof(double) : sizeof(float));
    CudaArray points(cu, numPoints, 4*elementSize, "points");
    CudaArray potential(cu, numPoints, elementSize, "potential");

    // Copy the grid points to the GPU.

    if (cu.getUseDoublePrecision()) {
        vector<double4> p(numPoints);
        for (int i = 0; i < numPoints; i++)
            p[i] = make_double4(inputGrid[i][0], inputGrid[i][1], inputGrid[i][2], 0);
        points.upload(p);
    }
    else {
        vector<float4> p(numPoints);
        for (int i = 0; i < numPoints; i++)
            p[i] = make_float4((float) inputGrid[i][0], (float) inputGrid[i][1], (float) inputGrid[i][2], 0);
        points.upload(p);
    }

    // Compute the potential.

    void* computePotentialArgs[] = {&cu.getPosq().getDevicePointer(), &labFrameDipoles->getDevicePointer(),
        &labFrameQuadrupoles->getDevicePointer(), &inducedDipole->getDevicePointer(), &points.getDevicePointer(),
        &potential.getDevicePointer(), &numPoints, cu.getPeriodicBoxSizePointer(), cu.getInvPeriodicBoxSizePointer(),
        cu.getPeriodicBoxVecXPointer(), cu.getPeriodicBoxVecYPointer(), cu.getPeriodicBoxVecZPointer()};
    int blockSize = 128;
    cu.executeKernel(computePotentialKernel, computePotentialArgs, numPoints, blockSize, blockSize*15*elementSize);
    outputElectrostaticPotential.resize(numPoints);
    if (cu.getUseDoublePrecision())
        potential.download(outputElectrostaticPotential);
    else {
        vector<float> p(numPoints);
        potential.download(p);
        for (int i = 0; i < numPoints; i++)
            outputElectrostaticPotential[i] = p[i];
    }
}

template <class T, class T4, class M4>
void CudaCalcMBPolElectrostaticsForceKernel::computeSystemMultipoleMoments(ContextImpl& context, vector<double>& outputMultipoleMoments) {
    // Compute the local coordinates relative to the center of mass.
    int numAtoms = cu.getNumAtoms();
    vector<T4> posq;
    vector<M4> velm;
    cu.getPosq().download(posq);
    cu.getVelm().download(velm);
    double totalMass = 0.0;
    Vec3 centerOfMass(0, 0, 0);
    for (int i = 0; i < numAtoms; i++) {
        double mass = (velm[i].w > 0 ? 1.0/velm[i].w : 0.0);
        totalMass += mass;
        centerOfMass[0] += mass*posq[i].x;
        centerOfMass[1] += mass*posq[i].y;
        centerOfMass[2] += mass*posq[i].z;
    }
    if (totalMass > 0.0) {
        centerOfMass[0] /= totalMass;
        centerOfMass[1] /= totalMass;
        centerOfMass[2] /= totalMass;
    }
    vector<double4> posqLocal(numAtoms);
    for (int i = 0; i < numAtoms; i++) {
        posqLocal[i].x = posq[i].x - centerOfMass[0];
        posqLocal[i].y = posq[i].y - centerOfMass[1];
        posqLocal[i].z = posq[i].z - centerOfMass[2];
        posqLocal[i].w = posq[i].w;
    }

    // Compute the multipole moments.

    double totalCharge = 0.0;
    double xdpl = 0.0;
    double ydpl = 0.0;
    double zdpl = 0.0;
    double xxqdp = 0.0;
    double xyqdp = 0.0;
    double xzqdp = 0.0;
    double yxqdp = 0.0;
    double yyqdp = 0.0;
    double yzqdp = 0.0;
    double zxqdp = 0.0;
    double zyqdp = 0.0;
    double zzqdp = 0.0;
    vector<T> labDipoleVec, inducedDipoleVec, quadrupoleVec;
    labFrameDipoles->download(labDipoleVec);
    inducedDipole->download(inducedDipoleVec);
    labFrameQuadrupoles->download(quadrupoleVec);
    for (int i = 0; i < numAtoms; i++) {
        totalCharge += posqLocal[i].w;
        double netDipoleX = (labDipoleVec[3*i]    + inducedDipoleVec[3*i]);
        double netDipoleY = (labDipoleVec[3*i+1]  + inducedDipoleVec[3*i+1]);
        double netDipoleZ = (labDipoleVec[3*i+2]  + inducedDipoleVec[3*i+2]);
        xdpl += posqLocal[i].x*posqLocal[i].w + netDipoleX;
        ydpl += posqLocal[i].y*posqLocal[i].w + netDipoleY;
        zdpl += posqLocal[i].z*posqLocal[i].w + netDipoleZ;
        xxqdp += posqLocal[i].x*posqLocal[i].x*posqLocal[i].w + 2*posqLocal[i].x*netDipoleX;
        xyqdp += posqLocal[i].x*posqLocal[i].y*posqLocal[i].w + posqLocal[i].x*netDipoleY + posqLocal[i].y*netDipoleX;
        xzqdp += posqLocal[i].x*posqLocal[i].z*posqLocal[i].w + posqLocal[i].x*netDipoleZ + posqLocal[i].z*netDipoleX;
        yxqdp += posqLocal[i].y*posqLocal[i].x*posqLocal[i].w + posqLocal[i].y*netDipoleX + posqLocal[i].x*netDipoleY;
        yyqdp += posqLocal[i].y*posqLocal[i].y*posqLocal[i].w + 2*posqLocal[i].y*netDipoleY;
        yzqdp += posqLocal[i].y*posqLocal[i].z*posqLocal[i].w + posqLocal[i].y*netDipoleZ + posqLocal[i].z*netDipoleY;
        zxqdp += posqLocal[i].z*posqLocal[i].x*posqLocal[i].w + posqLocal[i].z*netDipoleX + posqLocal[i].x*netDipoleZ;
        zyqdp += posqLocal[i].z*posqLocal[i].y*posqLocal[i].w + posqLocal[i].z*netDipoleY + posqLocal[i].y*netDipoleZ;
        zzqdp += posqLocal[i].z*posqLocal[i].z*posqLocal[i].w + 2*posqLocal[i].z*netDipoleZ;
    }

    // Convert the quadrupole from traced to traceless form.

    double qave = (xxqdp + yyqdp + zzqdp)/3;
    xxqdp = 1.5*(xxqdp-qave);
    xyqdp = 1.5*xyqdp;
    xzqdp = 1.5*xzqdp;
    yxqdp = 1.5*yxqdp;
    yyqdp = 1.5*(yyqdp-qave);
    yzqdp = 1.5*yzqdp;
    zxqdp = 1.5*zxqdp;
    zyqdp = 1.5*zyqdp;
    zzqdp = 1.5*(zzqdp-qave);

    // Add the traceless atomic quadrupoles to the total quadrupole moment.

    for (int i = 0; i < numAtoms; i++) {
        xxqdp = xxqdp + 3*quadrupoleVec[5*i];
        xyqdp = xyqdp + 3*quadrupoleVec[5*i+1];
        xzqdp = xzqdp + 3*quadrupoleVec[5*i+2];
        yxqdp = yxqdp + 3*quadrupoleVec[5*i+1];
        yyqdp = yyqdp + 3*quadrupoleVec[5*i+3];
        yzqdp = yzqdp + 3*quadrupoleVec[5*i+4];
        zxqdp = zxqdp + 3*quadrupoleVec[5*i+2];
        zyqdp = zyqdp + 3*quadrupoleVec[5*i+4];
        zzqdp = zzqdp + -3*(quadrupoleVec[5*i]+quadrupoleVec[5*i+3]);
    }

    double debye = 4.80321;
    outputMultipoleMoments.resize(13);
    outputMultipoleMoments[0] = totalCharge;
    outputMultipoleMoments[1] = 10.0*xdpl*debye;
    outputMultipoleMoments[2] = 10.0*ydpl*debye;
    outputMultipoleMoments[3] = 10.0*zdpl*debye;
    outputMultipoleMoments[4] = 100.0*xxqdp*debye;
    outputMultipoleMoments[5] = 100.0*xyqdp*debye;
    outputMultipoleMoments[6] = 100.0*xzqdp*debye;
    outputMultipoleMoments[7] = 100.0*yxqdp*debye;
    outputMultipoleMoments[8] = 100.0*yyqdp*debye;
    outputMultipoleMoments[9] = 100.0*yzqdp*debye;
    outputMultipoleMoments[10] = 100.0*zxqdp*debye;
    outputMultipoleMoments[11] = 100.0*zyqdp*debye;
    outputMultipoleMoments[12] = 100.0*zzqdp*debye;
}

void CudaCalcMBPolElectrostaticsForceKernel::getSystemMultipoleMoments(ContextImpl& context, vector<double>& outputMultipoleMoments) {
    ensureMultipolesValid(context);
    if (cu.getUseDoublePrecision())
        computeSystemMultipoleMoments<double, double4, double4>(context, outputMultipoleMoments);
    else if (cu.getUseMixedPrecision())
        computeSystemMultipoleMoments<float, float4, double4>(context, outputMultipoleMoments);
    else
        computeSystemMultipoleMoments<float, float4, float4>(context, outputMultipoleMoments);
}

void CudaCalcMBPolElectrostaticsForceKernel::copyParametersToContext(ContextImpl& context, const AmoebaMultipoleForce& force) {
    // Make sure the new parameters are acceptable.

    cu.setAsCurrent();
    if (force.getNumMultipoles() != cu.getNumAtoms())
        throw OpenMMException("updateParametersInContext: The number of multipoles has changed");

    // Record the per-multipole parameters.

    cu.getPosq().download(cu.getPinnedBuffer());
    float4* posqf = (float4*) cu.getPinnedBuffer();
    double4* posqd = (double4*) cu.getPinnedBuffer();
    vector<float2> dampingAndTholeVec;
    vector<float> polarizabilityVec;
    vector<float> molecularDipolesVec;
    vector<float> molecularQuadrupolesVec;
    vector<int4> multipoleParticlesVec;
    for (int i = 0; i < force.getNumMultipoles(); i++) {
        double charge, thole, damping, polarity;
        int axisType, atomX, atomY, atomZ;
        vector<double> dipole, quadrupole;
        force.getMultipoleParameters(i, charge, dipole, quadrupole, axisType, atomZ, atomX, atomY, thole, damping, polarity);
        if (cu.getUseDoublePrecision())
            posqd[i].w = charge;
        else
            posqf[i].w = (float) charge;
        dampingAndTholeVec.push_back(make_float2((float) damping, (float) thole));
        polarizabilityVec.push_back((float) polarity);
        multipoleParticlesVec.push_back(make_int4(atomX, atomY, atomZ, axisType));
        for (int j = 0; j < 3; j++)
            molecularDipolesVec.push_back((float) dipole[j]);
        molecularQuadrupolesVec.push_back((float) quadrupole[0]);
        molecularQuadrupolesVec.push_back((float) quadrupole[1]);
        molecularQuadrupolesVec.push_back((float) quadrupole[2]);
        molecularQuadrupolesVec.push_back((float) quadrupole[4]);
        molecularQuadrupolesVec.push_back((float) quadrupole[5]);
    }
    if (!hasQuadrupoles) {
        for (int i = 0; i < (int) molecularQuadrupolesVec.size(); i++)
            if (molecularQuadrupolesVec[i] != 0.0)
                throw OpenMMException("updateParametersInContext: Cannot set a non-zero quadrupole moment, because quadrupoles were excluded from the kernel");
    }
    for (int i = force.getNumMultipoles(); i < cu.getPaddedNumAtoms(); i++) {
        dampingAndTholeVec.push_back(make_float2(0, 0));
        polarizabilityVec.push_back(0);
        multipoleParticlesVec.push_back(make_int4(0, 0, 0, 0));
        for (int j = 0; j < 3; j++)
            molecularDipolesVec.push_back(0);
        for (int j = 0; j < 5; j++)
            molecularQuadrupolesVec.push_back(0);
    }
    dampingAndThole->upload(dampingAndTholeVec);
    polarizability->upload(polarizabilityVec);
    multipoleParticles->upload(multipoleParticlesVec);
    molecularDipoles->upload(molecularDipolesVec);
    molecularQuadrupoles->upload(molecularQuadrupolesVec);
    cu.getPosq().upload(cu.getPinnedBuffer());
    cu.invalidateMolecules();
    multipolesAreValid = false;
}