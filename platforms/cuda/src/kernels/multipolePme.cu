#define ARRAY(x,y) array[(x)-1+((y)-1)*PME_ORDER]

/**
 * Calculate the spline coefficients for a single atom along a single axis.
 */
__device__ void computeBSplinePoint(real4* thetai, real w, real* array) {
    // initialization to get to 2nd order recursion

    ARRAY(2,2) = w;
    ARRAY(2,1) = 1 - w;

    // perform one pass to get to 3rd order recursion

    ARRAY(3,3) = 0.5f * w * ARRAY(2,2);
    ARRAY(3,2) = 0.5f * ((1+w)*ARRAY(2,1)+(2-w)*ARRAY(2,2));
    ARRAY(3,1) = 0.5f * (1-w) * ARRAY(2,1);

    // compute standard B-spline recursion to desired order

    for (int i = 4; i <= PME_ORDER; i++)
    {
        int k = i - 1;
        real denom = RECIP(k);
        ARRAY(i,i) = denom * w * ARRAY(k,k);
        for (int j = 1; j <= i-2; j++)
            ARRAY(i,i-j) = denom * ((w+j)*ARRAY(k,i-j-1)+(i-j-w)*ARRAY(k,i-j));
        ARRAY(i,1) = denom * (1-w) * ARRAY(k,1);
    }

    // get coefficients for the B-spline first derivative

    int k = PME_ORDER - 1;
    ARRAY(k,PME_ORDER) = ARRAY(k,PME_ORDER-1);
    for (int i = PME_ORDER-1; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);

    // get coefficients for the B-spline second derivative

    k = PME_ORDER - 2;
    ARRAY(k,PME_ORDER-1) = ARRAY(k,PME_ORDER-2);
    for (int i = PME_ORDER-2; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);
    ARRAY(k,PME_ORDER) = ARRAY(k,PME_ORDER-1);
    for (int i = PME_ORDER-1; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);

    // get coefficients for the B-spline third derivative

    k = PME_ORDER - 3;
    ARRAY(k,PME_ORDER-2) = ARRAY(k,PME_ORDER-3);
    for (int i = PME_ORDER-3; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);
    ARRAY(k,PME_ORDER-1) = ARRAY(k,PME_ORDER-2);
    for (int i = PME_ORDER-2; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);
    ARRAY(k,PME_ORDER) = ARRAY(k,PME_ORDER-1);
    for (int i = PME_ORDER-1; i >= 2; i--)
        ARRAY(k,i) = ARRAY(k,i-1) - ARRAY(k,i);
    ARRAY(k,1) = -ARRAY(k,1);

    // copy coefficients from temporary to permanent storage

    for (int i = 1; i <= PME_ORDER; i++)
        thetai[i-1] = make_real4(ARRAY(PME_ORDER,i), ARRAY(PME_ORDER-1,i), ARRAY(PME_ORDER-2,i), ARRAY(PME_ORDER-3,i));
}

/**
 * Compute the index of the grid point each atom is associated with.
 */
extern "C" __global__ void findAtomGridIndex(const real4* __restrict__ posq, int2* __restrict__ pmeAtomGridIndex,
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        real4 pos = posq[i];
        pos -= periodicBoxVecZ*floor(pos.z*recipBoxVecZ.z+0.5f);
        pos -= periodicBoxVecY*floor(pos.y*recipBoxVecY.z+0.5f);
        pos -= periodicBoxVecX*floor(pos.x*recipBoxVecX.z+0.5f);

        // First axis.

        real w = pos.x*recipBoxVecX.x+pos.y*recipBoxVecY.x+pos.z*recipBoxVecZ.x;
        real fr = GRID_SIZE_X*(w-(int)(w+0.5f)+0.5f);
        int ifr = (int) fr;
        int igrid1 = ifr-PME_ORDER+1;

        // Second axis.

        w = pos.y*recipBoxVecY.y+pos.z*recipBoxVecZ.y;
        fr = GRID_SIZE_Y*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) fr;
        int igrid2 = ifr-PME_ORDER+1;

        // Third axis.

        w = pos.z*recipBoxVecZ.z;
        fr = GRID_SIZE_Z*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) fr;
        int igrid3 = ifr-PME_ORDER+1;

        // Record the grid point.

        igrid1 += (igrid1 < 0 ? GRID_SIZE_X : 0);
        igrid2 += (igrid2 < 0 ? GRID_SIZE_Y : 0);
        igrid3 += (igrid3 < 0 ? GRID_SIZE_Z : 0);
        pmeAtomGridIndex[i] = make_int2(i, igrid1*GRID_SIZE_Y*GRID_SIZE_Z+igrid2*GRID_SIZE_Z+igrid3);
    }
}

/**
 * Convert the potential from fractional to Cartesian coordinates.
 */
extern "C" __global__ void transformPotentialToCartesianCoordinates(const real* __restrict__ fphi, real* __restrict__ cphi, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    // Build matrices for transforming the potential.

    __shared__ real a[3][3];
    if (threadIdx.x == 0) {
        a[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        a[1][0] = GRID_SIZE_X*recipBoxVecY.x;
        a[2][0] = GRID_SIZE_X*recipBoxVecZ.x;
        a[0][1] = GRID_SIZE_Y*recipBoxVecX.y;
        a[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        a[2][1] = GRID_SIZE_Y*recipBoxVecZ.y;
        a[0][2] = GRID_SIZE_Z*recipBoxVecX.z;
        a[1][2] = GRID_SIZE_Z*recipBoxVecY.z;
        a[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    int index1[] = {0, 1, 2, 0, 0, 1};
    int index2[] = {0, 1, 2, 1, 2, 2};
    __shared__ real b[6][6];
    if (threadIdx.x < 36) {
        int i = threadIdx.x/6;
        int j = threadIdx.x-6*i;
        b[i][j] = a[index1[i]][index1[j]]*a[index2[i]][index2[j]];
        if (index1[j] != index2[j])
            b[i][j] += (i < 3 ? b[i][j] : a[index1[i]][index2[j]]*a[index2[i]][index1[j]]);
    }
    __syncthreads();

    // Transform the potential.
    
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        cphi[10*i] = fphi[20*i];
        cphi[10*i+1] = a[0][0]*fphi[20*i+1] + a[0][1]*fphi[20*i+2] + a[0][2]*fphi[20*i+3];
        cphi[10*i+2] = a[1][0]*fphi[20*i+1] + a[1][1]*fphi[20*i+2] + a[1][2]*fphi[20*i+3];
        cphi[10*i+3] = a[2][0]*fphi[20*i+1] + a[2][1]*fphi[20*i+2] + a[2][2]*fphi[20*i+3];
        for (int j = 0; j < 6; j++) {
            cphi[10*i+4+j] = 0;
            for (int k = 0; k < 6; k++)
                cphi[10*i+4+j] += b[j][k]*fphi[20*i+4+k];
        }
    }
}

extern "C" __global__ void gridSpreadFixedMultipoles(const real4* __restrict__ posq,
        real2* __restrict__ pmeGrid, int2* __restrict__ pmeAtomGridIndex,
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    real array[PME_ORDER*PME_ORDER];
    real4 theta1[PME_ORDER];
    real4 theta2[PME_ORDER];
    real4 theta3[PME_ORDER];
    
    // Process the atoms in spatially sorted order.  This improves cache performance when loading
    // the grid values.
    
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        int m = pmeAtomGridIndex[i].x;
        real4 pos = posq[m];
        pos -= periodicBoxVecZ*floor(pos.z*recipBoxVecZ.z+0.5f);
        pos -= periodicBoxVecY*floor(pos.y*recipBoxVecY.z+0.5f);
        pos -= periodicBoxVecX*floor(pos.x*recipBoxVecX.z+0.5f);

        // Since we need the full set of thetas, it's faster to compute them here than load them
        // from global memory.

        real w = pos.x*recipBoxVecX.x+pos.y*recipBoxVecY.x+pos.z*recipBoxVecZ.x;
        real fr = GRID_SIZE_X*(w-(int)(w+0.5f)+0.5f);
        int ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid1 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta1, w, array);
        w = pos.y*recipBoxVecY.y+pos.z*recipBoxVecZ.y;
        fr = GRID_SIZE_Y*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid2 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta2, w, array);
        w = pos.z*recipBoxVecZ.z;
        fr = GRID_SIZE_Z*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid3 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta3, w, array);
        igrid1 += (igrid1 < 0 ? GRID_SIZE_X : 0);
        igrid2 += (igrid2 < 0 ? GRID_SIZE_Y : 0);
        igrid3 += (igrid3 < 0 ? GRID_SIZE_Z : 0);
        
        // Spread the charge from this atom onto each grid point.
         
        for (int ix = 0; ix < PME_ORDER; ix++) {
            int xbase = igrid1+ix;
            xbase -= (xbase >= GRID_SIZE_X ? GRID_SIZE_X : 0);
            xbase = xbase*GRID_SIZE_Y*GRID_SIZE_Z;
            real4 t = theta1[ix];
            
            for (int iy = 0; iy < PME_ORDER; iy++) {
                int ybase = igrid2+iy;
                ybase -= (ybase >= GRID_SIZE_Y ? GRID_SIZE_Y : 0);
                ybase = xbase + ybase*GRID_SIZE_Z;
                real4 u = theta2[iy];
                
                for (int iz = 0; iz < PME_ORDER; iz++) {
                    int zindex = igrid3+iz;
                    zindex -= (zindex >= GRID_SIZE_Z ? GRID_SIZE_Z : 0);
                    int index = ybase + zindex;
                    real4 v = theta3[iz];

                    real atomCharge = pos.w;
                    real term0 = atomCharge*u.x*v.x;
                    real add = term0*t.x;
#ifdef USE_DOUBLE_PRECISION
                    unsigned long long * ulonglong_p = (unsigned long long *) pmeGrid;
                    atomicAdd(&ulonglong_p[2*index],  static_cast<unsigned long long>((long long) (add*0x100000000)));
#else
                    atomicAdd(&pmeGrid[index].x, add);
#endif
                }
            }
        }
    }
}

extern "C" __global__ void gridSpreadInducedDipoles(const real4* __restrict__ posq, const real* __restrict__ inducedDipole,
        const real* __restrict__ inducedDipolePolar, real2* __restrict__ pmeGrid, int2* __restrict__ pmeAtomGridIndex,
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    real array[PME_ORDER*PME_ORDER];
    real4 theta1[PME_ORDER];
    real4 theta2[PME_ORDER];
    real4 theta3[PME_ORDER];
    __shared__ real cartToFrac[3][3];
    if (threadIdx.x == 0) {
        cartToFrac[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        cartToFrac[0][1] = GRID_SIZE_X*recipBoxVecY.x;
        cartToFrac[0][2] = GRID_SIZE_X*recipBoxVecZ.x;
        cartToFrac[1][0] = GRID_SIZE_Y*recipBoxVecX.y;
        cartToFrac[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        cartToFrac[1][2] = GRID_SIZE_Y*recipBoxVecZ.y;
        cartToFrac[2][0] = GRID_SIZE_Z*recipBoxVecX.z;
        cartToFrac[2][1] = GRID_SIZE_Z*recipBoxVecY.z;
        cartToFrac[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    
    // Process the atoms in spatially sorted order.  This improves cache performance when loading
    // the grid values.
    
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        int m = pmeAtomGridIndex[i].x;
        real4 pos = posq[m];
        pos -= periodicBoxVecZ*floor(pos.z*recipBoxVecZ.z+0.5f);
        pos -= periodicBoxVecY*floor(pos.y*recipBoxVecY.z+0.5f);
        pos -= periodicBoxVecX*floor(pos.x*recipBoxVecX.z+0.5f);

        // Since we need the full set of thetas, it's faster to compute them here than load them
        // from global memory.

        real w = pos.x*recipBoxVecX.x+pos.y*recipBoxVecY.x+pos.z*recipBoxVecZ.x;
        real fr = GRID_SIZE_X*(w-(int)(w+0.5f)+0.5f);
        int ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid1 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta1, w, array);
        w = pos.y*recipBoxVecY.y+pos.z*recipBoxVecZ.y;
        fr = GRID_SIZE_Y*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid2 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta2, w, array);
        w = pos.z*recipBoxVecZ.z;
        fr = GRID_SIZE_Z*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid3 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta3, w, array);
        igrid1 += (igrid1 < 0 ? GRID_SIZE_X : 0);
        igrid2 += (igrid2 < 0 ? GRID_SIZE_Y : 0);
        igrid3 += (igrid3 < 0 ? GRID_SIZE_Z : 0);
        
        // Spread the charge from this atom onto each grid point.
         
        for (int ix = 0; ix < PME_ORDER; ix++) {
            int xbase = igrid1+ix;
            xbase -= (xbase >= GRID_SIZE_X ? GRID_SIZE_X : 0);
            xbase = xbase*GRID_SIZE_Y*GRID_SIZE_Z;
            real4 t = theta1[ix];
            
            for (int iy = 0; iy < PME_ORDER; iy++) {
                int ybase = igrid2+iy;
                ybase -= (ybase >= GRID_SIZE_Y ? GRID_SIZE_Y : 0);
                ybase = xbase + ybase*GRID_SIZE_Z;
                real4 u = theta2[iy];
                
                for (int iz = 0; iz < PME_ORDER; iz++) {
                    int zindex = igrid3+iz;
                    zindex -= (zindex >= GRID_SIZE_Z ? GRID_SIZE_Z : 0);
                    int index = ybase + zindex;
                    real4 v = theta3[iz];

                    real3 cinducedDipole = make_real3(inducedDipole[m*3], inducedDipole[m*3+1], inducedDipole[m*3+2]);
                    real3 cinducedDipolePolar = make_real3(inducedDipolePolar[m*3], inducedDipolePolar[m*3+1], inducedDipolePolar[m*3+2]);
                    real3 finducedDipole = make_real3(cinducedDipole.x*cartToFrac[0][0] + cinducedDipole.y*cartToFrac[0][1] + cinducedDipole.z*cartToFrac[0][2],
                                                      cinducedDipole.x*cartToFrac[1][0] + cinducedDipole.y*cartToFrac[1][1] + cinducedDipole.z*cartToFrac[1][2],
                                                      cinducedDipole.x*cartToFrac[2][0] + cinducedDipole.y*cartToFrac[2][1] + cinducedDipole.z*cartToFrac[2][2]);
                    real3 finducedDipolePolar = make_real3(cinducedDipolePolar.x*cartToFrac[0][0] + cinducedDipolePolar.y*cartToFrac[0][1] + cinducedDipolePolar.z*cartToFrac[0][2],
                                                           cinducedDipolePolar.x*cartToFrac[1][0] + cinducedDipolePolar.y*cartToFrac[1][1] + cinducedDipolePolar.z*cartToFrac[1][2],
                                                           cinducedDipolePolar.x*cartToFrac[2][0] + cinducedDipolePolar.y*cartToFrac[2][1] + cinducedDipolePolar.z*cartToFrac[2][2]);
                    real term01 = finducedDipole.y*u.y*v.x + finducedDipole.z*u.x*v.y;
                    real term11 = finducedDipole.x*u.x*v.x;
                    real term02 = finducedDipolePolar.y*u.y*v.x + finducedDipolePolar.z*u.x*v.y;
                    real term12 = finducedDipolePolar.x*u.x*v.x;
                    real add1 = term01*t.x + term11*t.y;
                    real add2 = term02*t.x + term12*t.y;
#ifdef USE_DOUBLE_PRECISION
                    unsigned long long * ulonglong_p = (unsigned long long *) pmeGrid;
                    atomicAdd(&ulonglong_p[2*index],  static_cast<unsigned long long>((long long) (add1*0x100000000)));
                    atomicAdd(&ulonglong_p[2*index+1],  static_cast<unsigned long long>((long long) (add2*0x100000000)));
#else
                    atomicAdd(&pmeGrid[index].x, add1);
                    atomicAdd(&pmeGrid[index].y, add2);
#endif
                }
            }
        }
    }
}

/**
 * In double precision, we have to use fixed point to accumulate the grid values, so convert them to floating point.
 */
extern "C" __global__ void finishSpreadCharge(long long* __restrict__ pmeGrid) {
    real* floatGrid = (real*) pmeGrid;
    const unsigned int gridSize = 2*GRID_SIZE_X*GRID_SIZE_Y*GRID_SIZE_Z;
    real scale = 1/(real) 0x100000000;
    for (int index = blockIdx.x*blockDim.x+threadIdx.x; index < gridSize; index += blockDim.x*gridDim.x)
        floatGrid[index] = scale*pmeGrid[index];
}

extern "C" __global__ void reciprocalConvolution(real2* __restrict__ pmeGrid, const real* __restrict__ pmeBsplineModuliX,
        const real* __restrict__ pmeBsplineModuliY, const real* __restrict__ pmeBsplineModuliZ, real4 periodicBoxSize,
        real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    const unsigned int gridSize = GRID_SIZE_X*GRID_SIZE_Y*GRID_SIZE_Z;
    real expFactor = M_PI*M_PI/(EWALD_ALPHA*EWALD_ALPHA);
    real scaleFactor = RECIP(M_PI*periodicBoxSize.x*periodicBoxSize.y*periodicBoxSize.z);
    for (int index = blockIdx.x*blockDim.x+threadIdx.x; index < gridSize; index += blockDim.x*gridDim.x) {
        int kx = index/(GRID_SIZE_Y*GRID_SIZE_Z);
        int remainder = index-kx*GRID_SIZE_Y*GRID_SIZE_Z;
        int ky = remainder/GRID_SIZE_Z;
        int kz = remainder-ky*GRID_SIZE_Z;
        if (kx == 0 && ky == 0 && kz == 0) {
            pmeGrid[index] = make_real2(0, 0);
            continue;
        }
        int mx = (kx < (GRID_SIZE_X+1)/2) ? kx : (kx-GRID_SIZE_X);
        int my = (ky < (GRID_SIZE_Y+1)/2) ? ky : (ky-GRID_SIZE_Y);
        int mz = (kz < (GRID_SIZE_Z+1)/2) ? kz : (kz-GRID_SIZE_Z);
        real mhx = mx*recipBoxVecX.x;
        real mhy = mx*recipBoxVecY.x+my*recipBoxVecY.y;
        real mhz = mx*recipBoxVecZ.x+my*recipBoxVecZ.y+mz*recipBoxVecZ.z;
        real bx = pmeBsplineModuliX[kx];
        real by = pmeBsplineModuliY[ky];
        real bz = pmeBsplineModuliZ[kz];
        real2 grid = pmeGrid[index];
        real m2 = mhx*mhx+mhy*mhy+mhz*mhz;
        real denom = m2*bx*by*bz;
        real eterm = scaleFactor*EXP(-expFactor*m2)/denom;
        pmeGrid[index] = make_real2(grid.x*eterm, grid.y*eterm);
    }
}

extern "C" __global__ void computeFixedPotentialFromGrid(const real2* __restrict__ pmeGrid, real* __restrict__ phi,
        long long* __restrict__ fieldBuffers, long long* __restrict__ fieldPolarBuffers,  const real4* __restrict__ posq,
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ,
        real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ, int2* __restrict__ pmeAtomGridIndex) {
    real array[PME_ORDER*PME_ORDER];
    real4 theta1[PME_ORDER];
    real4 theta2[PME_ORDER];
    real4 theta3[PME_ORDER];
    __shared__ real fracToCart[3][3];
    if (threadIdx.x == 0) {
        fracToCart[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        fracToCart[1][0] = GRID_SIZE_X*recipBoxVecY.x;
        fracToCart[2][0] = GRID_SIZE_X*recipBoxVecZ.x;
        fracToCart[0][1] = GRID_SIZE_Y*recipBoxVecX.y;
        fracToCart[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        fracToCart[2][1] = GRID_SIZE_Y*recipBoxVecZ.y;
        fracToCart[0][2] = GRID_SIZE_Z*recipBoxVecX.z;
        fracToCart[1][2] = GRID_SIZE_Z*recipBoxVecY.z;
        fracToCart[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    
    // Process the atoms in spatially sorted order.  This improves cache performance when loading
    // the grid values.
    
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        int m = pmeAtomGridIndex[i].x;
        real4 pos = posq[m];
        pos -= periodicBoxVecZ*floor(pos.z*recipBoxVecZ.z+0.5f);
        pos -= periodicBoxVecY*floor(pos.y*recipBoxVecY.z+0.5f);
        pos -= periodicBoxVecX*floor(pos.x*recipBoxVecX.z+0.5f);

        // Since we need the full set of thetas, it's faster to compute them here than load them
        // from global memory.

        real w = pos.x*recipBoxVecX.x+pos.y*recipBoxVecY.x+pos.z*recipBoxVecZ.x;
        real fr = GRID_SIZE_X*(w-(int)(w+0.5f)+0.5f);
        int ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid1 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta1, w, array);
        w = pos.y*recipBoxVecY.y+pos.z*recipBoxVecZ.y;
        fr = GRID_SIZE_Y*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid2 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta2, w, array);
        w = pos.z*recipBoxVecZ.z;
        fr = GRID_SIZE_Z*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid3 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta3, w, array);
        igrid1 += (igrid1 < 0 ? GRID_SIZE_X : 0);
        igrid2 += (igrid2 < 0 ? GRID_SIZE_Y : 0);
        igrid3 += (igrid3 < 0 ? GRID_SIZE_Z : 0);

        // Compute the potential from this grid point.

        real tuv000 = 0;
        real tuv001 = 0;
        real tuv010 = 0;
        real tuv100 = 0;
        real tuv200 = 0;
        real tuv020 = 0;
        real tuv002 = 0;
        real tuv110 = 0;
        real tuv101 = 0;
        real tuv011 = 0;
        real tuv300 = 0;
        real tuv030 = 0;
        real tuv003 = 0;
        real tuv210 = 0;
        real tuv201 = 0;
        real tuv120 = 0;
        real tuv021 = 0;
        real tuv102 = 0;
        real tuv012 = 0;
        real tuv111 = 0;
        for (int iz = 0; iz < PME_ORDER; iz++) {
            int k = igrid3+iz-(igrid3+iz >= GRID_SIZE_Z ? GRID_SIZE_Z : 0);
            real4 v = theta3[iz];
            real tu00 = 0;
            real tu10 = 0;
            real tu01 = 0;
            real tu20 = 0;
            real tu11 = 0;
            real tu02 = 0;
            real tu30 = 0;
            real tu21 = 0;
            real tu12 = 0;
            real tu03 = 0;
            for (int iy = 0; iy < PME_ORDER; iy++) {
                int j = igrid2+iy-(igrid2+iy >= GRID_SIZE_Y ? GRID_SIZE_Y : 0);
                real4 u = theta2[iy];
                real4 t = make_real4(0, 0, 0, 0);
                for (int ix = 0; ix < PME_ORDER; ix++) {
                    int i = igrid1+ix-(igrid1+ix >= GRID_SIZE_X ? GRID_SIZE_X : 0);
                    int gridIndex = i*GRID_SIZE_Y*GRID_SIZE_Z + j*GRID_SIZE_Z + k;
                    real tq = pmeGrid[gridIndex].x;
                    real4 tadd = theta1[ix];
                    t.x += tq*tadd.x;
                    t.y += tq*tadd.y;
                    t.z += tq*tadd.z;
                    t.w += tq*tadd.w;
                }
                tu00 += t.x*u.x;
                tu10 += t.y*u.x;
                tu01 += t.x*u.y;
                tu20 += t.z*u.x;
                tu11 += t.y*u.y;
                tu02 += t.x*u.z;
                tu30 += t.w*u.x;
                tu21 += t.z*u.y;
                tu12 += t.y*u.z;
                tu03 += t.x*u.w;
            }
            tuv000 += tu00*v.x;
            tuv100 += tu10*v.x;
            tuv010 += tu01*v.x;
            tuv001 += tu00*v.y;
            tuv200 += tu20*v.x;
            tuv020 += tu02*v.x;
            tuv002 += tu00*v.z;
            tuv110 += tu11*v.x;
            tuv101 += tu10*v.y;
            tuv011 += tu01*v.y;
            tuv300 += tu30*v.x;
            tuv030 += tu03*v.x;
            tuv003 += tu00*v.w;
            tuv210 += tu21*v.x;
            tuv201 += tu20*v.y;
            tuv120 += tu12*v.x;
            tuv021 += tu02*v.y;
            tuv102 += tu10*v.z;
            tuv012 += tu01*v.z;
            tuv111 += tu11*v.y;
        }
        phi[20*m] = tuv000;
        phi[20*m+1] = tuv100;
        phi[20*m+2] = tuv010;
        phi[20*m+3] = tuv001;
        phi[20*m+4] = tuv200;
        phi[20*m+5] = tuv020;
        phi[20*m+6] = tuv002;
        phi[20*m+7] = tuv110;
        phi[20*m+8] = tuv101;
        phi[20*m+9] = tuv011;
        phi[20*m+10] = tuv300;
        phi[20*m+11] = tuv030;
        phi[20*m+12] = tuv003;
        phi[20*m+13] = tuv210;
        phi[20*m+14] = tuv201;
        phi[20*m+15] = tuv120;
        phi[20*m+16] = tuv021;
        phi[20*m+17] = tuv102;
        phi[20*m+18] = tuv012;
        phi[20*m+19] = tuv111;
        real dipoleScale = (4/(real) 3)*(EWALD_ALPHA*EWALD_ALPHA*EWALD_ALPHA)/SQRT_PI;
        long long fieldx = (long long) ((-tuv100*fracToCart[0][0]-tuv010*fracToCart[0][1]-tuv001*fracToCart[0][2])*0x100000000);
        fieldBuffers[m] = fieldx;
        fieldPolarBuffers[m] = fieldx;
        long long fieldy = (long long) ((-tuv100*fracToCart[1][0]-tuv010*fracToCart[1][1]-tuv001*fracToCart[1][2])*0x100000000);
        fieldBuffers[m+PADDED_NUM_ATOMS] = fieldy;
        fieldPolarBuffers[m+PADDED_NUM_ATOMS] = fieldy;
        long long fieldz = (long long) ((-tuv100*fracToCart[2][0]-tuv010*fracToCart[2][1]-tuv001*fracToCart[2][2])*0x100000000);
        fieldBuffers[m+2*PADDED_NUM_ATOMS] = fieldz;
        fieldPolarBuffers[m+2*PADDED_NUM_ATOMS] = fieldz;
    }
}

extern "C" __global__ void computeInducedPotentialFromGrid(const real2* __restrict__ pmeGrid, real* __restrict__ phid,
        real* __restrict__ phip, real* __restrict__ phidp, const real4* __restrict__ posq,
        real4 periodicBoxVecX, real4 periodicBoxVecY, real4 periodicBoxVecZ, real3 recipBoxVecX,
        real3 recipBoxVecY, real3 recipBoxVecZ, int2* __restrict__ pmeAtomGridIndex) {
    real array[PME_ORDER*PME_ORDER];
    real4 theta1[PME_ORDER];
    real4 theta2[PME_ORDER];
    real4 theta3[PME_ORDER];
    
    // Process the atoms in spatially sorted order.  This improves cache performance when loading
    // the grid values.
    
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        int m = pmeAtomGridIndex[i].x;
        real4 pos = posq[m];
        pos -= periodicBoxVecZ*floor(pos.z*recipBoxVecZ.z+0.5f);
        pos -= periodicBoxVecY*floor(pos.y*recipBoxVecY.z+0.5f);
        pos -= periodicBoxVecX*floor(pos.x*recipBoxVecX.z+0.5f);

        // Since we need the full set of thetas, it's faster to compute them here than load them
        // from global memory.

        real w = pos.x*recipBoxVecX.x+pos.y*recipBoxVecY.x+pos.z*recipBoxVecZ.x;
        real fr = GRID_SIZE_X*(w-(int)(w+0.5f)+0.5f);
        int ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid1 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta1, w, array);
        w = pos.y*recipBoxVecY.y+pos.z*recipBoxVecZ.y;
        fr = GRID_SIZE_Y*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid2 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta2, w, array);
        w = pos.z*recipBoxVecZ.z;
        fr = GRID_SIZE_Z*(w-(int)(w+0.5f)+0.5f);
        ifr = (int) floor(fr);
        w = fr - ifr;
        int igrid3 = ifr-PME_ORDER+1;
        computeBSplinePoint(theta3, w, array);
        igrid1 += (igrid1 < 0 ? GRID_SIZE_X : 0);
        igrid2 += (igrid2 < 0 ? GRID_SIZE_Y : 0);
        igrid3 += (igrid3 < 0 ? GRID_SIZE_Z : 0);

        // Compute the potential from this grid point.

        real tuv100_1 = 0;
        real tuv010_1 = 0;
        real tuv001_1 = 0;
        real tuv200_1 = 0;
        real tuv020_1 = 0;
        real tuv002_1 = 0;
        real tuv110_1 = 0;
        real tuv101_1 = 0;
        real tuv011_1 = 0;
        real tuv100_2 = 0;
        real tuv010_2 = 0;
        real tuv001_2 = 0;
        real tuv200_2 = 0;
        real tuv020_2 = 0;
        real tuv002_2 = 0;
        real tuv110_2 = 0;
        real tuv101_2 = 0;
        real tuv011_2 = 0;
        real tuv000 = 0;
        real tuv001 = 0;
        real tuv010 = 0;
        real tuv100 = 0;
        real tuv200 = 0;
        real tuv020 = 0;
        real tuv002 = 0;
        real tuv110 = 0;
        real tuv101 = 0;
        real tuv011 = 0;
        real tuv300 = 0;
        real tuv030 = 0;
        real tuv003 = 0;
        real tuv210 = 0;
        real tuv201 = 0;
        real tuv120 = 0;
        real tuv021 = 0;
        real tuv102 = 0;
        real tuv012 = 0;
        real tuv111 = 0;
        for (int iz = 0; iz < PME_ORDER; iz++) {
            int k = igrid3+iz-(igrid3+iz >= GRID_SIZE_Z ? GRID_SIZE_Z : 0);
            real4 v = theta3[iz];
            real tu00_1 = 0;
            real tu01_1 = 0;
            real tu10_1 = 0;
            real tu20_1 = 0;
            real tu11_1 = 0;
            real tu02_1 = 0;
            real tu00_2 = 0;
            real tu01_2 = 0;
            real tu10_2 = 0;
            real tu20_2 = 0;
            real tu11_2 = 0;
            real tu02_2 = 0;
            real tu00 = 0;
            real tu10 = 0;
            real tu01 = 0;
            real tu20 = 0;
            real tu11 = 0;
            real tu02 = 0;
            real tu30 = 0;
            real tu21 = 0;
            real tu12 = 0;
            real tu03 = 0;
            for (int iy = 0; iy < PME_ORDER; iy++) {
                int j = igrid2+iy-(igrid2+iy >= GRID_SIZE_Y ? GRID_SIZE_Y : 0);
                real4 u = theta2[iy];
                real t0_1 = 0;
                real t1_1 = 0;
                real t2_1 = 0;
                real t0_2 = 0;
                real t1_2 = 0;
                real t2_2 = 0;
                real t3 = 0;
                for (int ix = 0; ix < PME_ORDER; ix++) {
                    int i = igrid1+ix-(igrid1+ix >= GRID_SIZE_X ? GRID_SIZE_X : 0);
                    int gridIndex = i*GRID_SIZE_Y*GRID_SIZE_Z + j*GRID_SIZE_Z + k;
                    real2 tq = pmeGrid[gridIndex];
                    real4 tadd = theta1[ix];
                    t0_1 += tq.x*tadd.x;
                    t1_1 += tq.x*tadd.y;
                    t2_1 += tq.x*tadd.z;
                    t0_2 += tq.y*tadd.x;
                    t1_2 += tq.y*tadd.y;
                    t2_2 += tq.y*tadd.z;
                    t3 += (tq.x+tq.y)*tadd.w;
                }
                tu00_1 += t0_1*u.x;
                tu10_1 += t1_1*u.x;
                tu01_1 += t0_1*u.y;
                tu20_1 += t2_1*u.x;
                tu11_1 += t1_1*u.y;
                tu02_1 += t0_1*u.z;
                tu00_2 += t0_2*u.x;
                tu10_2 += t1_2*u.x;
                tu01_2 += t0_2*u.y;
                tu20_2 += t2_2*u.x;
                tu11_2 += t1_2*u.y;
                tu02_2 += t0_2*u.z;
                real t0 = t0_1 + t0_2;
                real t1 = t1_1 + t1_2;
                real t2 = t2_1 + t2_2;
                tu00 += t0*u.x;
                tu10 += t1*u.x;
                tu01 += t0*u.y;
                tu20 += t2*u.x;
                tu11 += t1*u.y;
                tu02 += t0*u.z;
                tu30 += t3*u.x;
                tu21 += t2*u.y;
                tu12 += t1*u.z;
                tu03 += t0*u.w;
            }
            tuv100_1 += tu10_1*v.x;
            tuv010_1 += tu01_1*v.x;
            tuv001_1 += tu00_1*v.y;
            tuv200_1 += tu20_1*v.x;
            tuv020_1 += tu02_1*v.x;
            tuv002_1 += tu00_1*v.z;
            tuv110_1 += tu11_1*v.x;
            tuv101_1 += tu10_1*v.y;
            tuv011_1 += tu01_1*v.y;
            tuv100_2 += tu10_2*v.x;
            tuv010_2 += tu01_2*v.x;
            tuv001_2 += tu00_2*v.y;
            tuv200_2 += tu20_2*v.x;
            tuv020_2 += tu02_2*v.x;
            tuv002_2 += tu00_2*v.z;
            tuv110_2 += tu11_2*v.x;
            tuv101_2 += tu10_2*v.y;
            tuv011_2 += tu01_2*v.y;
            tuv000 += tu00*v.x;
            tuv100 += tu10*v.x;
            tuv010 += tu01*v.x;
            tuv001 += tu00*v.y;
            tuv200 += tu20*v.x;
            tuv020 += tu02*v.x;
            tuv002 += tu00*v.z;
            tuv110 += tu11*v.x;
            tuv101 += tu10*v.y;
            tuv011 += tu01*v.y;
            tuv300 += tu30*v.x;
            tuv030 += tu03*v.x;
            tuv003 += tu00*v.w;
            tuv210 += tu21*v.x;
            tuv201 += tu20*v.y;
            tuv120 += tu12*v.x;
            tuv021 += tu02*v.y;
            tuv102 += tu10*v.z;
            tuv012 += tu01*v.z;
            tuv111 += tu11*v.y;
        }
        phid[10*m]   = 0;
        phid[10*m+1] = tuv100_1;
        phid[10*m+2] = tuv010_1;
        phid[10*m+3] = tuv001_1;
        phid[10*m+4] = tuv200_1;
        phid[10*m+5] = tuv020_1;
        phid[10*m+6] = tuv002_1;
        phid[10*m+7] = tuv110_1;
        phid[10*m+8] = tuv101_1;
        phid[10*m+9] = tuv011_1;

        phip[10*m]   = 0;
        phip[10*m+1] = tuv100_2;
        phip[10*m+2] = tuv010_2;
        phip[10*m+3] = tuv001_2;
        phip[10*m+4] = tuv200_2;
        phip[10*m+5] = tuv020_2;
        phip[10*m+6] = tuv002_2;
        phip[10*m+7] = tuv110_2;
        phip[10*m+8] = tuv101_2;
        phip[10*m+9] = tuv011_2;

        phidp[20*m] = tuv000;
        phidp[20*m+1] = tuv100;
        phidp[20*m+2] = tuv010;
        phidp[20*m+3] = tuv001;
        phidp[20*m+4] = tuv200;
        phidp[20*m+5] = tuv020;
        phidp[20*m+6] = tuv002;
        phidp[20*m+7] = tuv110;
        phidp[20*m+8] = tuv101;
        phidp[20*m+9] = tuv011;
        phidp[20*m+10] = tuv300;
        phidp[20*m+11] = tuv030;
        phidp[20*m+12] = tuv003;
        phidp[20*m+13] = tuv210;
        phidp[20*m+14] = tuv201;
        phidp[20*m+15] = tuv120;
        phidp[20*m+16] = tuv021;
        phidp[20*m+17] = tuv102;
        phidp[20*m+18] = tuv012;
        phidp[20*m+19] = tuv111;
    }
}

extern "C" __global__ void computeFixedMultipoleForceAndEnergy(real4* __restrict__ posq, unsigned long long* __restrict__ forceBuffers, unsigned long   long* __restrict__ potentialBuffers,
        real* __restrict__ energyBuffer,
        const real* __restrict__ phi_global, const real* __restrict__ cphi_global, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    real multipole[1];
    const int deriv1[] = {1, 4, 7, 8, 10, 15, 17, 13, 14, 19};
    const int deriv2[] = {2, 7, 5, 9, 13, 11, 18, 15, 19, 16};
    const int deriv3[] = {3, 8, 9, 6, 14, 16, 12, 19, 17, 18};
    real energy = 0;
    __shared__ real fracToCart[3][3];
    if (threadIdx.x == 0) {
        fracToCart[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        fracToCart[1][0] = GRID_SIZE_X*recipBoxVecY.x;
        fracToCart[2][0] = GRID_SIZE_X*recipBoxVecZ.x;
        fracToCart[0][1] = GRID_SIZE_Y*recipBoxVecX.y;
        fracToCart[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        fracToCart[2][1] = GRID_SIZE_Y*recipBoxVecZ.y;
        fracToCart[0][2] = GRID_SIZE_Z*recipBoxVecX.z;
        fracToCart[1][2] = GRID_SIZE_Z*recipBoxVecY.z;
        fracToCart[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        // Compute the torque.

        multipole[0] = posq[i].w;

        const real* cphi = &cphi_global[10*i];

        // Compute the force and energy.

        const real* phi = &phi_global[20*i];
        real4 f = make_real4(0, 0, 0, 0);
        for (int k = 0; k < 1; k++) {
            energy += multipole[k]*phi[k];
            f.x += multipole[k]*phi[deriv1[k]];
            f.y += multipole[k]*phi[deriv2[k]];
            f.z += multipole[k]*phi[deriv3[k]];
        }
        f = make_real4(EPSILON_FACTOR*(f.x*fracToCart[0][0] + f.y*fracToCart[0][1] + f.z*fracToCart[0][2]),
                       EPSILON_FACTOR*(f.x*fracToCart[1][0] + f.y*fracToCart[1][1] + f.z*fracToCart[1][2]),
                       EPSILON_FACTOR*(f.x*fracToCart[2][0] + f.y*fracToCart[2][1] + f.z*fracToCart[2][2]), 0);
        forceBuffers[i] -= static_cast<unsigned long long>((long long) (f.x*0x100000000));
        forceBuffers[i+PADDED_NUM_ATOMS] -= static_cast<unsigned long long>((long long) (f.y*0x100000000));
        forceBuffers[i+PADDED_NUM_ATOMS*2] -= static_cast<unsigned long long>((long long) (f.z*0x100000000));
        #ifdef INCLUDE_CHARGE_REDISTRIBUTION
            potentialBuffers[i] += static_cast<unsigned long long>((long long) (phi[0]*0x100000000));
        #endif

    }
    energyBuffer[blockIdx.x*blockDim.x+threadIdx.x] += 0.5f*EPSILON_FACTOR*energy;
}

extern "C" __global__ void computeInducedDipoleForceAndEnergy(real4* __restrict__ posq, unsigned long long* __restrict__ forceBuffers, unsigned long    long* __restrict__ potentialBuffers,
        real* __restrict__ energyBuffer,
        const real* __restrict__ inducedDipole_global, const real* __restrict__ inducedDipolePolar_global,
        const real* __restrict__ phi_global, const real* __restrict__ phid_global, const real* __restrict__ phip_global,
        const real* __restrict__ phidp_global, const real* __restrict__ cphi_global, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    real multipole[1];
    real cinducedDipole[3], inducedDipole[3];
    real cinducedDipolePolar[3], inducedDipolePolar[3];
    const int deriv1[] = {1, 4, 7, 8, 10, 15, 17, 13, 14, 19};
    const int deriv2[] = {2, 7, 5, 9, 13, 11, 18, 15, 19, 16};
    const int deriv3[] = {3, 8, 9, 6, 14, 16, 12, 19, 17, 18};
    real energy = 0;
    __shared__ real fracToCart[3][3];
    if (threadIdx.x == 0) {
        fracToCart[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        fracToCart[1][0] = GRID_SIZE_X*recipBoxVecY.x;
        fracToCart[2][0] = GRID_SIZE_X*recipBoxVecZ.x;
        fracToCart[0][1] = GRID_SIZE_Y*recipBoxVecX.y;
        fracToCart[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        fracToCart[2][1] = GRID_SIZE_Y*recipBoxVecZ.y;
        fracToCart[0][2] = GRID_SIZE_Z*recipBoxVecX.z;
        fracToCart[1][2] = GRID_SIZE_Z*recipBoxVecY.z;
        fracToCart[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        // Compute the torque.

        multipole[0] = posq[i].w;
        const real* cphi = &cphi_global[10*i];

        // Compute the force and energy.

        cinducedDipole[0] = inducedDipole_global[i*3];
        cinducedDipole[1] = inducedDipole_global[i*3+1];
        cinducedDipole[2] = inducedDipole_global[i*3+2];
        cinducedDipolePolar[0] = inducedDipolePolar_global[i*3];
        cinducedDipolePolar[1] = inducedDipolePolar_global[i*3+1];
        cinducedDipolePolar[2] = inducedDipolePolar_global[i*3+2];
        
        // Multiply the dipoles by cartToFrac, which is just the transpose of fracToCart.
        
        inducedDipole[0] = cinducedDipole[0]*fracToCart[0][0] + cinducedDipole[1]*fracToCart[1][0] + cinducedDipole[2]*fracToCart[2][0];
        inducedDipole[1] = cinducedDipole[0]*fracToCart[0][1] + cinducedDipole[1]*fracToCart[1][1] + cinducedDipole[2]*fracToCart[2][1];
        inducedDipole[2] = cinducedDipole[0]*fracToCart[0][2] + cinducedDipole[1]*fracToCart[1][2] + cinducedDipole[2]*fracToCart[2][2];
        inducedDipolePolar[0] = cinducedDipolePolar[0]*fracToCart[0][0] + cinducedDipolePolar[1]*fracToCart[1][0] + cinducedDipolePolar[2]*fracToCart[2][0];
        inducedDipolePolar[1] = cinducedDipolePolar[0]*fracToCart[0][1] + cinducedDipolePolar[1]*fracToCart[1][1] + cinducedDipolePolar[2]*fracToCart[2][1];
        inducedDipolePolar[2] = cinducedDipolePolar[0]*fracToCart[0][2] + cinducedDipolePolar[1]*fracToCart[1][2] + cinducedDipolePolar[2]*fracToCart[2][2];
        const real* phi = &phi_global[20*i];
        const real* phip = &phip_global[10*i];
        const real* phid = &phid_global[10*i];
        real4 f = make_real4(0, 0, 0, 0);

        energy += inducedDipole[0]*phi[1];
        energy += inducedDipole[1]*phi[2];
        energy += inducedDipole[2]*phi[3];

        for (int k = 0; k < 3; k++) {
            int j1 = deriv1[k+1];
            int j2 = deriv2[k+1];
            int j3 = deriv3[k+1];
            f.x += (inducedDipole[k]+inducedDipolePolar[k])*phi[j1];
            f.y += (inducedDipole[k]+inducedDipolePolar[k])*phi[j2];
            f.z += (inducedDipole[k]+inducedDipolePolar[k])*phi[j3];
#ifndef DIRECT_POLARIZATION
            f.x += (inducedDipole[k]*phip[j1] + inducedDipolePolar[k]*phid[j1]);
            f.y += (inducedDipole[k]*phip[j2] + inducedDipolePolar[k]*phid[j2]);
            f.z += (inducedDipole[k]*phip[j3] + inducedDipolePolar[k]*phid[j3]);
#endif
        }

        const real* phidp = &phidp_global[20*i];
        //for (int k = 0; k < 10; k++) {
        for (int k = 0; k < 1; k++) {
            f.x += multipole[k]*phidp[deriv1[k]];
            f.y += multipole[k]*phidp[deriv2[k]];
            f.z += multipole[k]*phidp[deriv3[k]];
        }
        f = make_real4(0.5f*EPSILON_FACTOR*(f.x*fracToCart[0][0] + f.y*fracToCart[0][1] + f.z*fracToCart[0][2]),
                       0.5f*EPSILON_FACTOR*(f.x*fracToCart[1][0] + f.y*fracToCart[1][1] + f.z*fracToCart[1][2]),
                       0.5f*EPSILON_FACTOR*(f.x*fracToCart[2][0] + f.y*fracToCart[2][1] + f.z*fracToCart[2][2]), 0);
        forceBuffers[i] -= static_cast<unsigned long long>((long long) (f.x*0x100000000));
        forceBuffers[i+PADDED_NUM_ATOMS] -= static_cast<unsigned long long>((long long) (f.y*0x100000000));
        forceBuffers[i+PADDED_NUM_ATOMS*2] -= static_cast<unsigned long long>((long long) (f.z*0x100000000));
        #ifdef INCLUDE_CHARGE_REDISTRIBUTION
            potentialBuffers[i] += static_cast<unsigned long long>((long long) (.5 * phidp[0]*0x100000000));
        #endif
    }
    energyBuffer[blockIdx.x*blockDim.x+threadIdx.x] += 0.5f*EPSILON_FACTOR*energy;
}

extern "C" __global__ void recordInducedFieldDipoles(const real* __restrict__ phid, real* const __restrict__ phip,
        long long* __restrict__ inducedField, long long* __restrict__ inducedFieldPolar, real3 recipBoxVecX, real3 recipBoxVecY, real3 recipBoxVecZ) {
    __shared__ real fracToCart[3][3];
    if (threadIdx.x == 0) {
        fracToCart[0][0] = GRID_SIZE_X*recipBoxVecX.x;
        fracToCart[1][0] = GRID_SIZE_X*recipBoxVecY.x;
        fracToCart[2][0] = GRID_SIZE_X*recipBoxVecZ.x;
        fracToCart[0][1] = GRID_SIZE_Y*recipBoxVecX.y;
        fracToCart[1][1] = GRID_SIZE_Y*recipBoxVecY.y;
        fracToCart[2][1] = GRID_SIZE_Y*recipBoxVecZ.y;
        fracToCart[0][2] = GRID_SIZE_Z*recipBoxVecX.z;
        fracToCart[1][2] = GRID_SIZE_Z*recipBoxVecY.z;
        fracToCart[2][2] = GRID_SIZE_Z*recipBoxVecZ.z;
    }
    __syncthreads();
    for (int i = blockIdx.x*blockDim.x+threadIdx.x; i < NUM_ATOMS; i += blockDim.x*gridDim.x) {
        inducedField[i] -= (long long) (0x100000000*(phid[10*i+1]*fracToCart[0][0] + phid[10*i+2]*fracToCart[0][1] + phid[10*i+3]*fracToCart[0][2]));
        inducedField[i+PADDED_NUM_ATOMS] -= (long long) (0x100000000*(phid[10*i+1]*fracToCart[1][0] + phid[10*i+2]*fracToCart[1][1] + phid[10*i+3]*fracToCart[1][2]));
        inducedField[i+PADDED_NUM_ATOMS*2] -= (long long) (0x100000000*(phid[10*i+1]*fracToCart[2][0] + phid[10*i+2]*fracToCart[2][1] + phid[10*i+3]*fracToCart[2][2]));
        inducedFieldPolar[i] -= (long long) (0x100000000*(phip[10*i+1]*fracToCart[0][0] + phip[10*i+2]*fracToCart[0][1] + phip[10*i+3]*fracToCart[0][2]));
        inducedFieldPolar[i+PADDED_NUM_ATOMS] -= (long long) (0x100000000*(phip[10*i+1]*fracToCart[1][0] + phip[10*i+2]*fracToCart[1][1] + phip[10*i+3]*fracToCart[1][2]));
        inducedFieldPolar[i+PADDED_NUM_ATOMS*2] -= (long long) (0x100000000*(phip[10*i+1]*fracToCart[2][0] + phip[10*i+2]*fracToCart[2][1] + phip[10*i+3]*fracToCart[2][2]));
    }
}
