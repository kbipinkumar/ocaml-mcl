/*   (C) Copyright 2001-2022 Stijn van Dongen
 *
 * This file is part of MCL.  You can redistribute and/or modify MCL under the
 * terms of the GNU General Public License; either version 3 of the License or
 * (at your option) any later version.  You should have received a copy of the
 * GPL along with MCL, in the file COPYING.
*/

/* TODO
 *    rand index, randc (Hubert et ??)

   (n:2) + 2 Sum{i=1->c1} Sum{j=1->c2} (n_ij:2) - [ Sum{i=1->c1} (n_i+:2) + Sum{j=1->c2} (n_+j:2) ]
                                 -------------------------
                                       n (n-1) : 2
     a + d
   ---------
     (n/2)

   a: same in C1 and same in C2
   d: different in C1 and different in C2.


            Sum{i=1->c1} Sum{j=1->c2} (n_ij/2) - [1/(n/2)] Sum{i=1->c1} (n_i/2) * Sum{j=1->c2} (n_j/2)
                                 -------------------------
 [1/2] *  ( Sum{i=1->c1} (n_i/2) + Sum{j=1->c2} (n_j/2) ) - [1/(n/2)] Sum{i=1->c1} (n_i/2) * Sum{j=1->c2} (n_j/2)

*/

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#include "clm.h"
#include "report.h"
#include "clmdist.h"

#include "impala/matrix.h"
#include "impala/io.h"
#include "impala/iface.h"
#include "impala/ivp.h"
#include "impala/app.h"
#include "impala/io.h"

#include "clew/clm.h"
#include "clew/cat.h"

#include "tingea/types.h"
#include "tingea/alloc.h"
#include "tingea/err.h"
#include "tingea/opt.h"
#include "tingea/minmax.h"



static double mclv_choose_sum
(  const mclv* vec
)  
   {  mclIvp* vecivps = vec->ivps
   ;  dim     vecsize = vec->n_ivps
   ;  double  sum  = 0.0

   ;  while (vecsize-- > 0)
      {  double val = (vecivps++)->val
      ;  double delta = val * (val - 1.0) * 0.5
      ;  if (delta > 0)
         sum += delta
   ;  }
      return sum
;  }


static double mclx_choose_sum
(  const mclx* mx
)
   {  dim i
   ;  double sum = 0.0
   ;  for (i=0;i<N_COLS(mx);i++)
      sum += mclv_choose_sum(mx->cols+i)
   ;  return sum
;  }


static double flt_decrement
(  pval     flt
,  void*    arg
)
   {  double factor = *((double*)arg)
   ;  if (flt > 1.01)
      return flt - 1.0
   ;  else
      return 0.01111111 * factor
         /* edges (their nodes) may never co-cluster
          * we put such edges at a sentinel value of about 0.011111.
          * The returned number will be divided by factor.
         */
;  }



enum
{  DIST_SPLITJOIN 
,  DIST_VARINF
,  DIST_MIRKIN
,  INDEX
}  ;


/*
 * clmdist will enstrict the clusterings, and possibly project them onto
 * the meet of the domains if necessary.
*/


static const char* me = "clm dist";


enum
{  DIST_OPT_OUTPUT = CLM_DISP_UNUSED
,  DIST_OPT_MODE
,  DIST_OPT_INDEX
,  DIST_OPT_CHAIN
,  DIST_OPT_SORT
,  DIST_OPT_NORMALISE
,  DIST_OPT_MCI
,  DIST_OPT_DIGITS
,  DIST_OPT_SELF
}  ;


enum
{  VOL_OPT_OUTPUT = CLM_DISP_UNUSED
,  VOL_OPT_IMX
,  VOL_OPT_RCL
,  VOL_OPT_SELF
,  VOL_OPT_GI
,  VOL_OPT_SKEW
}  ;

static mcxOptAnchor volOptions[] =
{  {  "-o"
   ,  MCX_OPT_HASARG
   ,  VOL_OPT_OUTPUT
   ,  "<fname>"
   ,  "output file name"
   }
,  {  "-rcl-skew"
   ,  MCX_OPT_HASARG | MCX_OPT_HIDDEN
   ,  VOL_OPT_SKEW
   ,  "<num>"
   ,  "skew x in [0-1] as x^num"
   }
,  {  "--self"
   ,  MCX_OPT_DEFAULT
   ,  VOL_OPT_SELF
   ,  NULL
   ,  "include self-compares (for rcl use)"
   }
,  {  "-gi"
   ,  MCX_OPT_HASARG
   ,  VOL_OPT_GI
   ,  "i/N"
   ,  "compute job i out of N jobs total"
   }
,  {  "-imx"
   ,  MCX_OPT_HASARG
   ,  VOL_OPT_IMX
   ,  "<fname>"
   ,  "read network"
   }
,  {  "-write-rcl"
   ,  MCX_OPT_HASARG
   ,  VOL_OPT_RCL
   ,  "<fname>"
   ,  "output (cross-ensemble) restricted contingency linkage matrix"
   }
,  {  NULL ,  0 ,  0 ,  NULL, NULL}
}  ;


static mcxOptAnchor distOptions[] =
{  {  "-o"
   ,  MCX_OPT_HASARG
   ,  DIST_OPT_OUTPUT
   ,  "<fname>"
   ,  "output file name"
   }
,  {  "-mode"
   ,  MCX_OPT_HASARG
   ,  DIST_OPT_MODE
   ,  "<mode>"
   ,  "one of sj|vi|mirkin"
   }
,  {  "--mci"
   ,  MCX_OPT_DEFAULT | MCX_OPT_HIDDEN
   ,  DIST_OPT_MCI
   ,  NULL
   ,  "output all against all matrix of distances"
   }
,  {  "--index"
   ,  MCX_OPT_DEFAULT
   ,  DIST_OPT_INDEX
   ,  NULL
   ,  "output Rand, corrected Rand and Jaccard index"
   }
,  {  "-digits"
   ,  MCX_OPT_HASARG
   ,  DIST_OPT_DIGITS
   ,  "<num>"
   ,  "number of trailing digits for floats"
   }
,  {  "--self"
   ,  MCX_OPT_DEFAULT | MCX_OPT_HIDDEN
   ,  DIST_OPT_SELF
   ,  NULL
   ,  "include self-compares (e.g. for heatmaps)"
   }
,  {  "--chain"
   ,  MCX_OPT_DEFAULT
   ,  DIST_OPT_CHAIN
   ,  NULL
   ,  "only compare consecutive clusterings"
   }
,  {  "--sort"
   ,  MCX_OPT_DEFAULT
   ,  DIST_OPT_SORT
   ,  NULL
   ,  "sort from coarse to fine-grained before computing distances"
   }
,  {  "--normalise"
   ,  MCX_OPT_DEFAULT | MCX_OPT_HIDDEN    /* not implemented */
   ,  DIST_OPT_NORMALISE
   ,  NULL
   ,  "NOT IMPLEMENTED normalise criterion (applicable for --vi and default split/join)"
   }
,  {  NULL ,  0 ,  0 ,  NULL, NULL}
}  ;


static mcxIO*  xfout    =  NULL;
static mcxIO*  xfrcl    =  NULL;
static mcxIO*  xfimx    =  NULL;
static int digits       =  -1;
static int mode_g       =  -1;
static mcxbool i_am_vol =  FALSE;   /* node faithfulness */
static mcxbool consecutive_g = FALSE;
static mcxbool mci_g    =  FALSE;
static mcxbool split_g  =  FALSE;
static mcxbool self_g   =  FALSE;
static mcxbool sort_g   =  FALSE;
static double skew_g    =  1.0;
static unsigned job_N   =  0;
static unsigned job_i   =  0;


static mcxstatus distInit
(  void
)
   {  xfout =  mcxIOnew("-", "w")
   ;  mode_g = 0
   ;  digits = 2
   ;  return STATUS_OK
;  }


static mcxstatus volInit
(  void
)
   {  xfout       =  mcxIOnew("-", "w")
   ;  xfrcl       =  mcxIOnew("out.rcl", "w")
   ;  i_am_vol    =  TRUE
   ;  consecutive_g = FALSE
   ;  return STATUS_OK
;  }


static mcxstatus volArgHandle
(  int optid
,  const char* val
)
   {  switch(optid)
      {  case VOL_OPT_OUTPUT
      :  mcxIOnewName(xfout, val)
      ;  break
      ;

         case VOL_OPT_SELF
      :  self_g = TRUE
      ;  break
      ;

         case VOL_OPT_SKEW
      :  skew_g = atof(val)
      ;  break
      ;

         case VOL_OPT_RCL
      :  mcxIOnewName(xfrcl, val)
      ;  break
      ;

         case VOL_OPT_GI
      :  if
         (  2 != sscanf(val, "%u/%u", &job_i, &job_N)
         || job_N == 0
         || job_i >= job_N
         )
         mcxDie(1, me, "unable to parse [%s] as i/N, i<N", val)
      ;  break
      ;

         case VOL_OPT_IMX
      :  xfimx = mcxIOnew(val, "r")
      ;  break
      ;

         default
      :  return STATUS_FAIL
      ;
      }
      return STATUS_OK
;  }


static mcxstatus distArgHandle
(  int optid
,  const char* val
)
   {  switch(optid)
      {  case DIST_OPT_OUTPUT
      :  mcxIOnewName(xfout, val)
      ;  break
      ;

         case DIST_OPT_DIGITS
      :  digits = atoi(val)
      ;  break
      ;

         case DIST_OPT_SELF
      :  self_g = TRUE
      ;  break
      ;

         case DIST_OPT_CHAIN
      :  consecutive_g = TRUE
      ;  break
      ;

         case DIST_OPT_MCI
      :  mci_g = TRUE
      ;  break
      ;

         case DIST_OPT_SORT
      :  sort_g = TRUE
      ;  break
      ;

         case DIST_OPT_MODE
      :     if (!strcmp(val, "sj"))
            mode_g = DIST_SPLITJOIN
         ;  else if (!strcmp(val, "vi"))
            mode_g = DIST_VARINF
         ;  else if
            (  !strcmp(val, "ehd")
            || !strcmp(val, "mk")
            || !strcmp(val, "mirkin")
            )
            mode_g = DIST_MIRKIN
         ;  else
            mcxDie(1, me, "unknown mode <%s>", val)
      ;  break
      ;

         case DIST_OPT_INDEX
      :  mode_g = INDEX
      ;  break
      ;

         default
      :  return STATUS_FAIL
      ;
      }
      return STATUS_OK
;  }


static double flt_add_if_left
(  pval lft
,  pval rgt
)
   {  return lft ?  lft + rgt : 0.0
;  }


static mcxstatus distMain
(  int                  argc
,  const char*          argv[]
)
   {  int               i
   ;  int a             =  0
   ;  mclx* vol_scores  =  NULL
   ;  mclx* mxrcl       =  NULL
   ;  double one        =  1.0
   ;  dim n_comparisons =  0
   ;  dim n_todo_total  =  0
   ;  dim n_clusterings =  0
   ;  dim n_thisjob     =  0
   ;  dim job_milestone =  0
   ;  mcxIO* xfin       =  mcxIOnew("-", "r")
   ;  mcxbits bits      =  MCLX_PRODUCE_PARTITION | MCLX_REQUIRE_DOMSTACK

   ;  mclxCat st
   ;  mclxCat st2

   ;  mclxCat *stptr = &st, *stptr1 = &st, *stptr2 = &st

   ;  mcxIO* xfdebug = mcxIOnew("-", "w")
   ;  mcxIOopen(xfdebug, EXIT_ON_FAIL)

   ;  mclxCatInit(&st)
   ;  mclxCatInit(&st2)

   ;  if (i_am_vol)
      me = "clm vol"
      
   ;  if (!mode_g)
      mode_g = DIST_SPLITJOIN

   ;  if (mode_g & DIST_MIRKIN || mode_g & DIST_SPLITJOIN)
      digits = 0

   ;  mcxIOopen(xfout, EXIT_ON_FAIL)

   ;  if (xfimx)
      {  mcxIOopen(xfimx, EXIT_ON_FAIL)
      ;  mcxIOopen(xfrcl, EXIT_ON_FAIL)
      ;  mxrcl = mclxReadx(xfimx, EXIT_ON_FAIL, MCLX_REQUIRE_GRAPH)
      ;  mclxUnary(mxrcl, fltxConst, &one)
      ;  mcxIOclose(xfimx)  /* fixme layout */
      ;  mcxIOfree(&xfimx)
   ;  }
      for (a=0;a<argc;a++)
      {  mcxstatus status
      ;  if (!strcmp(argv[a], "--"))
         {  stptr = &st2
         ;  split_g = TRUE
         ;  stptr2 = &st2
         ;  continue
      ;  }
         mcxIOnewName(xfin, argv[a])
      ;  status = mclxCatRead(xfin, stptr, 0, NULL, NULL, bits)
      ;  mcxIOclose(xfin)
      ;  if (status)
         break
;if (0)
         mclxWrite(stptr->level[stptr->n_level-1].mx, xfdebug, MCLXIO_VALUE_GETENV, RETURN_ON_FAIL)
   ;  }

      if (!a || a != argc)
      mcxDie(1, me, "failed to read one or more cluster files")

   ;  mcxIOfree(&xfdebug)

   ;  if (sort_g)                     /* fixme only works for single stack */
      mclxCatSortCoarseFirst(&st)

   ;  if (i_am_vol && st.n_level)
      vol_scores
      =  mclxCartesian
         (  mclvCanonical(NULL, 1, 1.0)
         ,  mclvClone(st.level[0].mx->dom_rows)
         ,  1.0
         )

   ;  if (mci_g)     /* tbcont idea is to output mcl matrix with distances */

   ;  n_todo_total =
      consecutive_g ? stptr1->n_level -1
      : split_g ? stptr1->n_level * stptr2->n_level
      : self_g ? stptr1->n_level + (stptr1->n_level * (stptr1->n_level-1)) / 2
      : (stptr1->n_level * (stptr1->n_level-1)) / 2
   ;  n_clusterings = 
      split_g ? stptr1->n_level + stptr2->n_level
      : stptr1->n_level

   ;  if (clm_progress_g && job_i == 0)
      mcxTell(me, "starting %d comparisons on %d clusterings", (int) n_todo_total, (int) n_clusterings)

   ;  for (i=0;i<stptr1->n_level;i++)
      {  mclx* c1       =  stptr1->level[i].mx
      ;  int j, jstart  =  split_g ? 0 : i+ (self_g ? 0 : 1)
      ;  for (j=jstart; j<stptr2->n_level;j++)     /* note stptr2 changes if split_g */
         {  mclx* c2  =  stptr2->level[j].mx
         ;  mclx* meet12, *meet21
         ;  double dist1d, dist2d
         ;  dim dist1i, dist2i
         ;  dim mod_id = job_N ? n_comparisons % job_N : 0

         ;  n_comparisons++
         ;  if (job_N && mod_id != job_i)
            continue

         ;  n_thisjob++
         ;  meet12 =  clmContingency(c1, c2)
         ;  meet21 =  mclxTranspose(meet12)

         ;  if (i_am_vol)
            {  dim k
            ;  if (clm_progress_g)
               {  if (!job_N)
                  {  if (i && j==jstart)
                     fputc('\n', stderr)
                  ;  fputc('.', stderr)
               ;  }
                  else    /* bit of nonsense, but I crave signs of progress */
                  {  dim mile = (n_thisjob * job_N * 10.0 -1.0) / (n_todo_total + job_N)
                  ;  if (mile > job_milestone)
                     {  fputc(" .,=+<>()-"[mile % 10], stderr)
                     ;  job_milestone = mile
                  ;  }
                  }
            ;  }
               for (k=0;k<N_COLS(meet12);k++)
               {  dim l
               ;  mclv* ct = meet12->cols+k        /* list of clusters contingent with c1 */
               ;  mclv* c1mem = c1->cols+k         /* the elements in c1 */
               ;  mclv* c2mem = NULL               /* we will loop through various c2 */

               ;  for (l=0;l<ct->n_ivps;l++)
                  {  ofs c2id = ct->ivps[l].idx    /* we have the id of this c2 */
                  ;  double meet_sz = ct->ivps[l].val
                  ;  c2mem = mclxGetVector(c2, c2id, EXIT_ON_FAIL, c2mem)
                  ;  mclp* tivp = NULL
                  ;  dim m
                  ;  mclv* meet = mcldMeet(c1mem, c2mem, NULL)
                  ;  mclv* nbvec = NULL
                  ;  dim minsize = MCX_MIN(c1mem->n_ivps, c2mem->n_ivps)
                                                   /* below is consistency, with skew generalisation (default 1.0 so off) */
                  ;  if (mxrcl)
                     mclvMakeConstant(meet, pow(1.0 * meet->n_ivps / (1.0 * minsize), skew_g))

                  ;  for (m=0;m<meet->n_ivps;m++)
                     {  dim thenode = meet->ivps[m].idx
                     ;  tivp = mclvGetIvp(vol_scores->cols+0, thenode, tivp)
                     ;  tivp->val += pow(1.0 * meet->n_ivps / (1.0 * minsize),skew_g)
                     ;  if (mxrcl)
                        {  nbvec = mclxGetVector(mxrcl, thenode, EXIT_ON_FAIL, nbvec)
                        ;  mclvBinary(nbvec, meet, nbvec, flt_add_if_left)
                     ;  }
                     }
                     mclvFree(&meet)
               ;  }
               }
               mclxFree(&meet12)  /* ugly, duplicate, due to continue. TBFxd */
            ;  mclxFree(&meet21)  /* ugly, duplicate, due to continue. TBFxd */
            ;  continue
         ;  }

           else
           {if (mode_g == DIST_SPLITJOIN)
               clmSJDistance(c1, c2, meet12, meet21, &dist1i, &dist2i)
            ,  fprintf
               (  xfout->fp
               ,  "d=%lu\td1=%lu\td2=%lu\tnn=%ld\tc1=%ld\tc2=%ld\tn1=%s\tn2=%s\t"
               ,  (ulong) (dist1i + dist2i)
               ,  (ulong) dist1i
               ,  (ulong) dist2i
               ,  (long) N_ROWS(c1)
               ,  (long) N_COLS(c1)
               ,  (long) N_COLS(c2)
               ,  stptr1->level[i].fname->str
               ,  stptr2->level[j].fname->str
               )

         ;  else if (mode_g == DIST_VARINF)
               clmVIDistance(c1, c2, meet12, &dist1d, &dist2d)
            ,  fprintf
               (  xfout->fp
               ,  "d=%.3f\td1=%.3f\td2=%.3f\tnn=%ld\tc1=%ld\tc2=%ld\tn1=%s\tn2=%s"
               ,  dist1d + dist2d
               ,  dist1d
               ,  dist2d
               ,  (long) N_ROWS(c1)
               ,  (long) N_COLS(c1)
               ,  (long) N_COLS(c2)
               ,  stptr1->level[i].fname->str
               ,  stptr2->level[j].fname->str
               )

         ;  else if (mode_g == DIST_MIRKIN)
               clmMKDistance(c1, c2, meet12, &dist1i, &dist2i)
            ,  fprintf
               (  xfout->fp
               ,  "d=%ld\td1=%ld\td2=%ld\tnn=%ld\tc1=%ld\tc2=%ld\tn1=%s\tn2=%s"
               ,  dist1i + dist2i
               ,  dist1i
               ,  dist2i
               ,  (long) N_ROWS(c1)
               ,  (long) N_COLS(c1)
               ,  (long) N_COLS(c2)
               ,  stptr1->level[i].fname->str
               ,  stptr2->level[j].fname->str
               )

         ;  else if (mode_g == INDEX)
            {  mclv* sizes_a  =  mclxColSizes(c1, MCL_VECTOR_SPARSE)
            ;  mclv* sizes_b  =  mclxColSizes(c2, MCL_VECTOR_SPARSE)
            ;  double soba    =  mclv_choose_sum(sizes_a)   /* sum of binomial */
            ;  double sobb    =  mclv_choose_sum(sizes_b)   /* sum of binomial */
            ;  double sobm    =  mclx_choose_sum(meet12)
            ;  double chsn_   =  (N_ROWS(c1) * 0.5 * (N_ROWS(c1) - 1.0))
            ;  double chsn    =  chsn_ ? chsn_ : 0.001   /* crazy, yes */

            ;  double rand    =  (chsn -  soba -  sobb +  2.0 * sobm) / chsn
            ;  double jaccard =  sobm / (soba + sobb - sobm)
            ;  double randc   =     (sobm - (soba * sobb) / chsn)
                                 /  ( 0.5 * (soba + sobb) - (soba * sobb) / chsn)
            ;  fprintf
               (  xfout->fp, "rand=%.5f jaccard=%.5f arand=%.5f n1=%s n2=%s"
               ,  rand, jaccard, randc
               ,  stptr1->level[i].fname->str ,  stptr2->level[j].fname->str
               )
         ;  }

            {  const char* e = getenv("CLMDIST_TAG")
            ;  if (e)
               fprintf(xfout->fp, "\ttag=%s", e)
            ;  fputc('\n', xfout->fp)
         ;  }
           }

            mclxFree(&meet12)
         ;  mclxFree(&meet21)
         ;  if (consecutive_g)
            break
      ;  }
      }

    ; if (clm_progress_g && job_i == 0)
      mcxTell(me, "[%d]", (int) n_comparisons)

    ; if (i_am_vol && n_comparisons)
      {  double factor = n_comparisons / 1000.0
      ;  mclxUnary(vol_scores, flt_decrement, &factor)
      ;  mclxUnary(vol_scores, fltxScale, &factor)

      ;  if (clm_progress_g && !job_N) fputc('\n', stderr)

      ;  mclxaWrite(vol_scores, xfout, 4, RETURN_ON_FAIL)
      ;  mclxFree(&vol_scores)
      ;  mcxIOclose(xfout)

      ;  if (mxrcl)
         {  mclxUnary(mxrcl, flt_decrement, &factor)
         ;  mclxUnary(mxrcl, fltxScale, &factor)
         ;  mclxWrite(mxrcl, xfrcl, 6, RETURN_ON_FAIL)
         ;  mcxIOfree(&xfrcl)
         ;  mclxFree(&mxrcl)
      ;  }
      }

      {  dim j
			;	 for (j=0;j<st.n_level;j++)
         {  mclxAnnot* an = st.level+j
         ;  mclxFree(&an->mx)
         ;  mcxTingFree(&an->fname)
      ;  }
         if (st.level) mcxFree(st.level)
			;	 for (j=0;j<st2.n_level;j++)
         {  mclxAnnot* an = st2.level+j
         ;  mclxFree(&an->mx)
         ;  mcxTingFree(&an->fname)
      ;  }
         if (st2.level) mcxFree(st2.level)
      ;  mcxIOfree(&xfin)
      ;  mcxIOfree(&xfout)    /* fixme: survey code for vol+dist consistent memory freeing */
   ;  }

      return STATUS_OK
;  }


mcxDispHook* mcxDispHookDist
(  void
)
   {  static mcxDispHook distEntry
   =  {  "dist"
      ,  "dist [options] <cl file>+"
      ,  distOptions
      ,  sizeof(distOptions)/sizeof(mcxOptAnchor) - 1
      ,  distArgHandle
      ,  distInit
      ,  distMain
      ,  0
      ,  -1
      ,  MCX_DISP_MANUAL
      }
   ;  return &distEntry
;  }


mcxDispHook* mcxDispHookVol
(  void
)
   {  static mcxDispHook volEntry
   =  {  "vol"
      ,  "vol [options] <cl file>+"
      ,  volOptions
      ,  sizeof(volOptions)/sizeof(mcxOptAnchor) - 1
      ,  volArgHandle
      ,  volInit
      ,  distMain
      ,  1
      ,  -1
      ,  MCX_DISP_MANUAL
      }
   ;  return &volEntry
;  }


