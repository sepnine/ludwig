/*
 * targetDP_CUDA.c: API Implementation for targetDP: CUDA version
 * Alan Gray, November 2013
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <math.h>

#include "targetDP.h"

void checkTargetError(const char *msg)
{
	cudaError_t err = cudaGetLastError();
	if( cudaSuccess != err) 
	{
		fprintf(stderr, "Cuda error: %s: %s.\n", msg, 
				cudaGetErrorString( err) );
		fflush(stdout);
		fflush(stderr);
		exit(EXIT_FAILURE);
	}                         
}

void targetMalloc(void **address_of_ptr,size_t size){

 
  cudaMalloc(address_of_ptr,size);
  checkTargetError("targetMalloc");

  return;
}

void targetCalloc(void **address_of_ptr,size_t size){

 
  cudaMalloc(address_of_ptr,size);
  double ZERO=0.;
  cudaMemset(*address_of_ptr, ZERO, size);
  checkTargetError("targetCalloc");

  return;
}

void copyToTarget(void *targetData,const void* data,size_t size){

  cudaMemcpy(targetData,data,size,cudaMemcpyHostToDevice);
  checkTargetError("copyToTarget");
  return;
}

void copyFromTarget(void *data,const void* targetData,size_t size){

  cudaMemcpy(data,targetData,size,cudaMemcpyDeviceToHost);
  checkTargetError("copyFromTarget");
  return;

}


__global__ static void copy_field_partial_gpu_d(double* f_out, const double* f_in, int nsites, int nfields, int *fullindex_d, int packedsize, int inpack) {

  int threadIndex;
  int i;


  

  /* CUDA thread index */
  threadIndex = blockIdx.x*blockDim.x+threadIdx.x;

  

  //Avoid going beyond problem domain
  //if ((threadIndex < nsites) && mask_d[threadIndex])

  if ((threadIndex < packedsize))
    {


      for (i=0;i<nfields;i++)
	{
	    
	  if (inpack)
	    f_out[i*nsites+fullindex_d[threadIndex]]
	    =f_in[i*packedsize+threadIndex];
	  else
	   f_out[i*packedsize+threadIndex]
	      =f_in[i*nsites+fullindex_d[threadIndex]];
	  
	}
    }


  return;
}
/* __global__ static void copy_field_partial_gpu_d(double* f_out, const double* f_in, int nsites, int nfields, char *mask_d, int *packedindex_d, int packedsize, int inpack) { */

/*   int threadIndex; */
/*   int i; */


  

/*   /\* CUDA thread index *\/ */
/*   threadIndex = blockIdx.x*blockDim.x+threadIdx.x; */

  

/*   //Avoid going beyond problem domain */
/*   if ((threadIndex < nsites) && mask_d[threadIndex]) */
/*     { */


/*       for (i=0;i<nfields;i++) */
/* 	{ */
	    
/* 	  if (inpack) */
/* 	    f_out[i*nsites+threadIndex] */
/* 	    =f_in[i*packedsize+packedindex_d[threadIndex]]; */
/* 	  else */
/* 	   f_out[i*packedsize+packedindex_d[threadIndex]] */
/* 	      =f_in[i*nsites+threadIndex]; */
	  
/* 	} */
/*     } */


/*   return; */
/* } */



void copyToTargetMasked(double *targetData,const double* data,size_t nsites,
			size_t nfields,char* siteMask){


  int i;
  int index;


  //allocate space TO BE  OPTIMISED
  int* fullindex;
  fullindex=(int*) malloc(nsites*sizeof(int));

  int* fullindex_d;
  cudaMalloc(&fullindex_d,nsites*sizeof(int));


  //get compression mapping
  int j=0;
  for (i=0; i<nsites; i++){
    if(siteMask[i]){
      fullindex[j]=i;
      //packedindex[i]=j;
      j++;
    }
    
  }

  int packedsize=j;




  double* tmpGrid;
  double* tmpGrid_d;
  cudaMalloc(&tmpGrid_d,packedsize*nfields*sizeof(double));
  tmpGrid = (double*) malloc(packedsize*nfields*sizeof(double));


  //copy compresssion info to GPU
  cudaMemcpy(fullindex_d, fullindex, packedsize*sizeof(int), cudaMemcpyHostToDevice);


    
  //compress grid
  for (i=0;i<nfields;i++)
    {
      
      j=0;
      for (index=0; index<nsites; index++){
	if(siteMask[index]){
	  tmpGrid[i*packedsize+j]=data[i*nsites+index];
	  j++;
	}
      }
      
    }


  
  //put compressed grid on GPU
  cudaMemcpy(tmpGrid_d, tmpGrid, packedsize*nfields*sizeof(double), cudaMemcpyHostToDevice); 



  //uncompress grid on GPU
  int nblocks=(packedsize+DEFAULT_TPB-1)/DEFAULT_TPB;
  copy_field_partial_gpu_d<<<nblocks,DEFAULT_TPB>>>(targetData,tmpGrid_d,nsites,
						    nfields,
						    fullindex_d, packedsize, 1);
  cudaThreadSynchronize();

  


  cudaFree(fullindex_d);
  cudaFree(tmpGrid_d);
  free(fullindex);
  free(tmpGrid);




  return;
  
}

void copyFromTargetMasked(double *data,const double* targetData,size_t nsites,
			size_t nfields,char* siteMask){




  int i;
  int index;


  //allocate space TO BE  OPTIMISED


  int* fullindex;
  fullindex=(int*) malloc(nsites*sizeof(int));

  int* fullindex_d;
  cudaMalloc(&fullindex_d,nsites*sizeof(int));


  //get compression mapping
  int j=0;
  for (i=0; i<nsites; i++){
    if(siteMask[i]){
      fullindex[j]=i;
      j++;
    }
    
  }

  int packedsize=j;




  double* tmpGrid;
  double* tmpGrid_d;
  cudaMalloc(&tmpGrid_d,packedsize*nfields*sizeof(double));
  tmpGrid = (double*) malloc(packedsize*nfields*sizeof(double));


  //copy compresssion info to GPU
  cudaMemcpy(fullindex_d, fullindex, packedsize*sizeof(int), cudaMemcpyHostToDevice);

  
  //compress grid on GPU
  int nblocks=(packedsize+DEFAULT_TPB-1)/DEFAULT_TPB;
  copy_field_partial_gpu_d<<<nblocks,DEFAULT_TPB>>>(tmpGrid_d,targetData,nsites,
						    nfields,
						    fullindex_d, packedsize, 0);
  cudaThreadSynchronize();

  //get compressed grid from GPU
  cudaMemcpy(tmpGrid, tmpGrid_d, packedsize*nfields*sizeof(double), cudaMemcpyDeviceToHost); 

    
  //expand into final grid
  for (i=0;i<nfields;i++)
    {
      
      j=0;
      for (index=0; index<nsites; index++){
	if(siteMask[index]){
	  data[i*nsites+index]=tmpGrid[i*packedsize+j];	
	  j++;
	}
      }
      
    }
  


  cudaFree(fullindex_d);
  cudaFree(tmpGrid_d);
  free(fullindex);
  free(tmpGrid);

  return;

}



void syncTarget(){
  cudaThreadSynchronize();
  checkTargetError("syncTarget");
  return;
}

void targetFree(void *ptr){
  
  cudaFree(ptr);
  checkTargetError("targetFree");
  return;
  
}
void copyConstantIntToTarget(int *data_d, int *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantIntToTarget");
  return;
} 
void copyConstantInt1DArrayToTarget(int *data_d, int *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantInt1DArrayToTarget");
  return;
} 
void copyConstantInt2DArrayToTarget(int **data_d, int *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantInt2DArrayToTarget");
  return;
} 
void copyConstantDoubleToTarget(double *data_d, double *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantDoubleToTarget");
  return;
} 
void copyConstantDouble1DArrayToTarget(double *data_d, double *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantDouble1DArrayToTarget");
  return;
} 

void copyConstantDouble2DArrayToTarget(double **data_d, double *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantDouble2DArrayToTarget");
  return;
} 
void copyConstantDouble3DArrayToTarget(double ***data_d, double *data, int size){
  cudaMemcpyToSymbol(*data_d, data, size, 0,cudaMemcpyHostToDevice);
  checkTargetError("copyConstantDouble3DArrayToTarget");
  return;
}

