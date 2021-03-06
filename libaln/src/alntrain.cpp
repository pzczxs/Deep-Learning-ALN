// ALN Library
// Copyright (C) 1995 - 2010 William W. Armstrong.
// 
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// Version 3 of the License, or (at your option) any later version.
// 
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
// 
// For further information contact 
// William W. Armstrong

// 3624 - 108 Street NW
// Edmonton, Alberta, Canada  T6J 1B4
// alntrain.cpp
// training support routines

///////////////////////////////////////////////////////////////////////////////
//  File version info:
// 
//  $Archive: /ALN Development/libaln/src/alntrain.cpp $
//  $Workfile: alntrain.cpp $
//  $Revision: 12 $
//  $Date: 8/18/07 2:51p $
//  $Author: Arms $
//
///////////////////////////////////////////////////////////////////////////////

#ifdef ALNDLL
#define ALNIMP __declspec(dllexport)
#endif

#include <aln.h>
#include "alnpriv.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// helper declarations
static int ALNAPI ValidateALNTrainInfo(const ALN* pALN,
                                       const ALNDATAINFO* pDataInfo,
                                       const ALNCALLBACKINFO* pCallbackInfo,
                                       int nMaxEpochs,
                                       double dblMinRMSErr,
                                       double dblLearnRate);

#ifdef _DEBUG
static void DebugValidateALNTrainInfo(const ALN* pALN,
                                      const ALNDATAINFO* pDataInfo,
                                      const ALNCALLBACKINFO* pCallbackInfo,
                                      int nMaxEpochs,
                                      double dblMinRMSErr,
                                      double dblLearnRate);
#endif

static int ALNAPI DoTrainALN(ALN* pALN,
                             const ALNDATAINFO* pDataInfo,
                             const ALNCALLBACKINFO* pCallbackInfo,
                             int nMaxEpochs,
                             double dblMinRMSErr,
                             double dblLearnRate,
                             BOOL bJitter);

// TrainALN expects data in monolithic array, row major order, ie,
//   row 0 col 0, row 0 col 1, ..., row 0 col n,
//   row 1 col 0, row 1 col 1, ..., row 1 col n,
//   ...,
//   row m col 0, row m col 1, ..., row m col n,
// if aVarInfo is NULL, then there must be nDim columns in data array
//   else aVarInfo must have pALN->nDim elements
// if adblData is NULL, then application must provide a training proc,
//   and must fill data vector during AN_FILLVECTOR message
// else adblData must have nPoints rows and nCols must be greater than zero
// nNotifyMask is the bitwise OR of all the notifications that should
//   sent during training, or AN_ALL, or AN_NONE
// returns ALN_* error code, (ALN_NOERROR on success)
ALNIMP int ALNAPI ALNTrain(ALN* pALN,
                           const ALNDATAINFO* pDataInfo,
                           const ALNCALLBACKINFO* pCallbackInfo,
                           int nMaxEpochs,
                           double dblMinRMSErr,
                           double dblLearnRate,
                           BOOL bJitter)
{
  int nReturn = ValidateALNTrainInfo(pALN, pDataInfo, pCallbackInfo,
                                     nMaxEpochs, dblMinRMSErr, dblLearnRate);
  if (nReturn == ALN_NOERROR)
  {
    // train if the ALN is successfully prepped
    if (PrepALN(pALN))
    {
      nReturn = DoTrainALN(pALN, pDataInfo, pCallbackInfo,
                           nMaxEpochs, dblMinRMSErr, dblLearnRate,
                           bJitter);
    }
    else
    {
      nReturn = ALN_GENERIC;
    }
  }
  return nReturn;
}


///////////////////////////////////////////////////////////////////////////////
// workhorse of TrainALN 
static int ALNAPI DoTrainALN(ALN* pALN,
                             const ALNDATAINFO* pDataInfo,
                             const ALNCALLBACKINFO* pCallbackInfo,
                             int nMaxEpochs,
                             double dblMinRMSErr,
                             double dblLearnRate,
                             BOOL bJitter)
{
#ifdef _DEBUG
  DebugValidateALNTrainInfo(pALN, pDataInfo, pCallbackInfo, nMaxEpochs, 
                            dblMinRMSErr, dblLearnRate);
#endif

  int nReturn = ALN_NOERROR;		    // assume success
    
	int nDim = pALN->nDim;
  int nPoints = pDataInfo->nPoints;
	
  ALNNODE* pTree = pALN->pTree;	    
	ASSERT(pTree != NULL);

  double* adblX;                    // input vector
	int* anShuffle = NULL;				    // point index shuffle array
  const double** apdblBase = NULL;  // data column base pointers
  CCutoffInfo* aCutoffInfo = NULL;  // eval cutoff speedup

  TRAININFO traininfo;					    // training info
	EPOCHINFO epochinfo;					    // epoch info
  TRAINDATA traindata;              // training data


  int nNotifyMask = (pCallbackInfo == NULL) ? AN_NONE : 
                                              pCallbackInfo->nNotifyMask;
  void* pvData = (pCallbackInfo == NULL) ? NULL : 
                                           pCallbackInfo->pvData;
  ALNNOTIFYPROC pfnNotifyProc = (pCallbackInfo == NULL) ? NULL : 
                                             pCallbackInfo->pfnNotifyProc;

	// init traindata
	traindata.dblLearnRate = dblLearnRate / nDim; // there is 1 adapt per input variable, so lower this
	traindata.nNotifyMask = nNotifyMask;
	traindata.pvData = pvData;
	traindata.pfnNotifyProc = pfnNotifyProc;

  // calc start and end points of training
  int nStart, nEnd; 
  CalcDataEndPoints(nStart, nEnd, pALN, pDataInfo);

	try	// main processing block
	{
    // allocate input vector
    adblX = new double[nDim];
    if (!adblX) ThrowALNMemoryException();
    memset(adblX, 0, sizeof(double) * nDim);

    // allocate column base vector
    apdblBase = AllocColumnBase(nStart, pALN, pDataInfo);

		// allocate and init shuffle array
		anShuffle = new int[nEnd - nStart + 1];
    if (!anShuffle) ThrowALNMemoryException();
		for (int i = nStart; i <= nEnd; i++)
			anShuffle[i - nStart] = i - nStart;

    // allocate and init cutoff info array
    aCutoffInfo = new CCutoffInfo[nEnd - nStart + 1];
    if (!aCutoffInfo) ThrowALNMemoryException();
		for (int i = nStart; i <= nEnd; i++)
			aCutoffInfo[i - nStart].pLFN = NULL;
    
		// count total number of LFNs in ALN
		int nLFNs = 0;
    int nAdaptedLFNs = 0;
    CountLFNs(pALN->pTree, nLFNs, nAdaptedLFNs);

		// init callback info 
		traininfo.nEpochs = nMaxEpochs;
		traininfo.nLFNs = nLFNs;
		traininfo.nActiveLFNs = 0;
		traininfo.dblRMSErr = 0.0;
	
		epochinfo.nEpoch = 0;
		epochinfo.nLFNs = nLFNs;
		epochinfo.nActiveLFNs = nAdaptedLFNs;
		epochinfo.dblEstRMSErr = 0.0;
    
		// notify beginning of training
		if (CanCallback(AN_TRAINSTART, pfnNotifyProc, nNotifyMask))
		{
      TRAININFO ti(traininfo);  // make copy to send!
      Callback(pALN, AN_TRAINSTART, &ti, pfnNotifyProc, pvData);
		}

		///// begin epoch loop
    int nResetCounters = 2;  // We reset counters for splitting when
                              //adaptation has had a chance to adjust pieces
															// This depends on epochsize, learning rate, RMS error, tolerance... etc.
															// We need to get a better way to compute this!!!!!!
		ResetCounters(pTree, pALN,TRUE); // now we need to initialize counters at start separately

    traininfo.dblRMSErr = dblMinRMSErr + 1.0;	// ...to enter epoch loop
		for (int nEpoch = 0; 
				 (nEpoch < nMaxEpochs) && (traininfo.dblRMSErr > dblMinRMSErr); 
				 nEpoch++)
		{
      int nCutoffs = 0;
           
      // notify beginning of epoch
			epochinfo.nEpoch = nEpoch;
			if (CanCallback(AN_EPOCHSTART, pfnNotifyProc, nNotifyMask))
			{
        EPOCHINFO ei(epochinfo);  // make copy to send!
				Callback(pALN, AN_EPOCHSTART, &ei, pfnNotifyProc, pvData);
			}

      // split candidate LFNs if not first epoch, and before counters
      // are reset
      if(nEpoch > 0 && (nEpoch%nResetCounters == 0))
      {
        //FindSplitLFN(pALN)now splits LFNs as it goes
        ALNNODE* pSplitLFN = FindSplitLFN(pALN);
        // old code when only one node was split: if (pSplitLFN != NULL)SplitLFN(pALN, pSplitLFN);
				// reset tree to mark useless pieces, etc
				// except for first epoch, where we mark everything as useful
				ResetCounters(pTree, pALN, nEpoch == 0);   
			}

      // track squared error
      double dblSqErrorSum = 0;	

      // we improve generalization by ensuring enough data points are available
      // for all the adapting LFNs in the system... when we add jitter, we can
      // "increase" the size of the training set by iterating over the input
      // points more than once before resetting the epoch stats
      int nRepCount = 1;
      for (int nRep = 0; nRep < nRepCount; nRep++)
      {
			  // shuffle training point indexes... in the case where
        // nStart != 0, these are offset counts from nStart
        Shuffle(nStart, nEnd, anShuffle);
      				    
			  int nPoint;
			  for (nPoint = nStart; nPoint <= nEnd; nPoint++)
			  {
          int nTrainPoint = anShuffle[nPoint - nStart]; // zero based
          ASSERT((nTrainPoint + nStart) <= nEnd);

          // fill input vector
          FillInputVector(pALN, adblX, nTrainPoint, nStart, apdblBase, 
                          pDataInfo, pCallbackInfo);
        
          // if we have first data point, init LFNs on first pass
          if (nEpoch == 0 && nPoint == nStart)
            InitLFNs(pTree, pALN, adblX);

          // jitter the data point
          if (bJitter)
            Jitter(pALN, adblX);

          // do an adapt eval to get active LFN and distance, and to prepare
          // tree for adaptation
          ALNNODE* pActiveLFN = NULL;
          CCutoffInfo& cutoffinfo = aCutoffInfo[nPoint - nStart];
          double dbl = AdaptEval(pTree, pALN, adblX, &cutoffinfo, &pActiveLFN);

 				  // track squared error before adapt, since adapt routines
          // do not relcalculate value of adapted surface
      	  dblSqErrorSum += dbl * dbl;

          // notify start of adapt
				  if (CanCallback(AN_ADAPTSTART, pfnNotifyProc, nNotifyMask))
				  {
            ADAPTINFO adaptinfo;
            adaptinfo.nAdapt = nPoint - nStart;
  				  adaptinfo.adblX = adblX;
  				  adaptinfo.dblErr = dbl;
					  Callback(pALN, AN_ADAPTSTART, &adaptinfo, pfnNotifyProc, pvData);
				  }

				  // do a useful adapt to correct any error
          traindata.dblGlobalError = dbl;
				  Adapt(pTree, pALN, adblX, 1.0, TRUE, &traindata);

				  // notify end of adapt
				  if (CanCallback(AN_ADAPTEND, pfnNotifyProc, nNotifyMask))
				  {
            ADAPTINFO adaptinfo;
            adaptinfo.nAdapt = nPoint - nStart;
  				  adaptinfo.adblX = adblX;
  				  adaptinfo.dblErr = dbl;
					  Callback(pALN, AN_ADAPTEND, &adaptinfo, pfnNotifyProc, pvData);
				  }
			  }	// end for each point in data set
      } // end reps

			// estimate RMS error on training set for this epoch
			epochinfo.dblEstRMSErr = sqrt(dblSqErrorSum / (nPoints * nRepCount));
						
			// calc true RMS if estimate below min, or if last epoch, or every
      // 10 epochs when jittering
			if (epochinfo.dblEstRMSErr <= dblMinRMSErr || nEpoch == (nMaxEpochs - 1) ||
          (bJitter && (nEpoch % 10 == 9)))
			{
        epochinfo.dblEstRMSErr = DoCalcRMSError(pALN, pDataInfo, pCallbackInfo);
			}

      // notify end of epoch
      nLFNs = nAdaptedLFNs = 0;
      CountLFNs(pALN->pTree, nLFNs, nAdaptedLFNs);
			epochinfo.nLFNs = nLFNs;
      epochinfo.nActiveLFNs = nAdaptedLFNs;

      // update train info, too
      traininfo.nEpochs = nEpoch;
		  traininfo.nLFNs = epochinfo.nLFNs;
      traininfo.nActiveLFNs = epochinfo.nActiveLFNs;
      traininfo.dblRMSErr = epochinfo.dblEstRMSErr;	// used to terminate epoch loop
			
      if (CanCallback(AN_EPOCHEND, pfnNotifyProc, nNotifyMask))
			{
        EPOCHINFO ei(epochinfo);  // make copy to send!
				Callback(pALN, AN_EPOCHEND, &ei, pfnNotifyProc, pvData);
			}
		} 
		///// end epoch loop

		// notify end of training
		
		if (CanCallback(AN_TRAINEND, pfnNotifyProc, nNotifyMask))
		{
      // don't bother copying traininfo, since this is the last message sent
      // we don't care if user changes it!
			Callback(pALN, AN_TRAINEND, &traininfo, pfnNotifyProc, pvData);
		}
	}
	catch (CALNUserException* e)	  // user abort exception
	{
		nReturn = ALN_USERABORT;
    e->Delete();
	}
	catch (CALNMemoryException* e)	// memory specific exceptions
	{
		nReturn = ALN_OUTOFMEM;
    e->Delete();
	}
	catch (CALNException* e)	      // anything other exception we recognize
	{
		nReturn = ALN_GENERIC;
    e->Delete();
	}
	catch (...)		                  // anything else, including FP errs
	{
		nReturn = ALN_GENERIC;
	}

	// deallocate mem
  delete[] adblX;
	delete[] anShuffle;
  delete[] aCutoffInfo;
  FreeColumnBase(apdblBase);

	return nReturn;
}

// validate ALNTRAININFO struct
static int ALNAPI ValidateALNTrainInfo(const ALN* pALN,
                                       const ALNDATAINFO* pDataInfo,
                                       const ALNCALLBACKINFO* pCallbackInfo,
                                       int nMaxEpochs,
                                       double dblMinRMSErr,
                                       double dblLearnRate)
{
  int nReturn = ValidateALNDataInfo(pALN, pDataInfo, pCallbackInfo);
  if (nReturn != ALN_NOERROR)
    return nReturn;

  // need at least one epoch
  if (nMaxEpochs <= 0)
  {
    return ALN_GENERIC;
  }

  // need non-negative error
  if (dblMinRMSErr < 0)
  {
    return ALN_GENERIC;
  }

  // learnrate should be positive and probably no more than than 0.5
	if (dblLearnRate < 0.0 || dblLearnRate > 0.5)
	{
		return ALN_GENERIC;
	}

  return ALN_NOERROR;
}

#ifdef _DEBUG
static void DebugValidateALNTrainInfo(const ALN* pALN,
                                      const ALNDATAINFO* pDataInfo,
                                      const ALNCALLBACKINFO* pCallbackInfo,
                                      int nMaxEpochs,
                                      double dblMinRMSErr,
                                      double dblLearnRate)
{
  DebugValidateALNDataInfo(pALN, pDataInfo, pCallbackInfo);
  ASSERT(nMaxEpochs > 0 && dblMinRMSErr >= 0 && dblLearnRate > 0.0 && dblLearnRate <= 0.5);
}
#endif
