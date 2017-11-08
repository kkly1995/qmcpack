#!/bin/bash -x

BUILD_DIR=$(pwd)
echo $BUILD_DIR

cat > $BUILD_TAG.pbs << EOF
#PBS -A MAT151
#PBS -N $BUILD_TAG
#PBS -j oe
#PBS -l walltime=1:00:00,nodes=1
#PBS -d $BUILD_DIR
#PBS -l partition=gpu

cd $BUILD_DIR

source /sw/rhea/environment-modules/3.2.10/rhel6.7_gnu4.4.7/init/bash

module unload PE-intel
module load PE-gnu/5.3.0-1.10.2
module load fftw
export FFTW_HOME=\$FFTW3_DIR
module load hdf5
module load git
module load cudatoolkit/8.0.44

env

module list

mkdir -p build

cd build

# real, full precision
cmake -DQMC_COMPLEX=0 -DQMC_MIXED_PRECISION=0 -DCMAKE_C_COMPILER="mpicc" -DCMAKE_CXX_COMPILER="mpicxx" -DCMAKE_CXX_FLAGS="-mno-bmi2 -mno-avx2" -DCMAKE_C_FLAGS="-mno-bmi2 -mno-avx2" -DBLAS_blas_LIBRARY="/usr/lib64/libblas.so.3" -DLAPACK_lapack_LIBRARY="/usr/lib64/atlas/liblapack.so.3" -DHDF5_INCLUDE_DIR="/sw/rhea/hdf5/1.8.11/rhel6.6_intel14.0.4/include" -DQMC_CUDA=1 .. 2>&1 | tee cmake.out

# hacky way to check on cmake. works for now
if ! ( grep -- '-- The C compiler identification is GNU 5.3.0' cmake.out && \
       grep -- '-- The CXX compiler identification is GNU 5.3.0' cmake.out ) ;
then
  echo "compiler version mismatch. exiting."
  exit 1
fi

# because Andreas tells me (and I observe) that GPU builds are unstable with Cmake
make -j 1
make -j 24

ctest -L unit


# real, mixed precision
rm -rf ./*
cmake -DQMC_COMPLEX=0 -DQMC_MIXED_PRECISION=1 -DCMAKE_C_COMPILER="mpicc" -DCMAKE_CXX_COMPILER="mpicxx" -DCMAKE_CXX_FLAGS="-mno-bmi2 -mno-avx2" -DCMAKE_C_FLAGS="-mno-bmi2 -mno-avx2" -DBLAS_blas_LIBRARY="/usr/lib64/libblas.so.3" -DLAPACK_lapack_LIBRARY="/usr/lib64/atlas/liblapack.so.3" -DHDF5_INCLUDE_DIR="/sw/rhea/hdf5/1.8.11/rhel6.6_intel14.0.4/include" -DQMC_CUDA=1 .. 2>&1 | tee cmake.out

make -j 1
make -j 24

ctest -L unit

# complex, full precision
rm -rf ./*
cmake -DQMC_COMPLEX=1 -DQMC_MIXED_PRECISION=0 -DCMAKE_C_COMPILER="mpicc" -DCMAKE_CXX_COMPILER="mpicxx" -DCMAKE_CXX_FLAGS="-mno-bmi2 -mno-avx2" -DCMAKE_C_FLAGS="-mno-bmi2 -mno-avx2" -DBLAS_blas_LIBRARY="/usr/lib64/libblas.so.3" -DLAPACK_lapack_LIBRARY="/usr/lib64/atlas/liblapack.so.3" -DHDF5_INCLUDE_DIR="/sw/rhea/hdf5/1.8.11/rhel6.6_intel14.0.4/include" -DQMC_CUDA=1 .. 2>&1 | tee cmake.out

make -j 1
make -j 24

ctest -L unit

# complex, mixed precision
rm -rf ./*
cmake -DQMC_COMPLEX=0 -DQMC_MIXED_PRECISION=0 -DCMAKE_C_COMPILER="mpicc" -DCMAKE_CXX_COMPILER="mpicxx" -DCMAKE_CXX_FLAGS="-mno-bmi2 -mno-avx2" -DCMAKE_C_FLAGS="-mno-bmi2 -mno-avx2" -DBLAS_blas_LIBRARY="/usr/lib64/libblas.so.3" -DLAPACK_lapack_LIBRARY="/usr/lib64/atlas/liblapack.so.3" -DHDF5_INCLUDE_DIR="/sw/rhea/hdf5/1.8.11/rhel6.6_intel14.0.4/include" -DQMC_CUDA=1 .. 2>&1 | tee cmake.out

make -j 1
make -j 24

ctest -L unit

EOF

cp $BUILD_TAG.pbs $BUILD_DIR

cd $BUILD_DIR

source scl_source enable rh-python35
which python

$BUILD_DIR/../../../scripts/blocking_qsub.py $BUILD_DIR $BUILD_TAG.pbs

## this end of job logic could probably be more elegant
## hacks to get us going

cp $BUILD_DIR/$BUILD_TAG.o* ../

# explicitly check for correct test output from all builds
[ $(grep '100% tests passed, 0 tests failed out of [0-9]*' ../$BUILD_TAG.o* | wc -l) -eq 4 ]
