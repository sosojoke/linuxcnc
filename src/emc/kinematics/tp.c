/********************************************************************
* Description: tp.c
*   Trajectory planner based on TC elements
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision$
* $Author$
* $Date$
********************************************************************/

#ifdef ULAPI
#include <stdio.h>
#include <stdlib.h>
#endif

#include "rtapi.h"		/* rtapi_print_msg */
#include "posemath.h"
#include "tc.h"
#include "tp.h"

// FIXME - debug only, remove later
#include <hal.h>
#include "../motion/motion.h"
#include "../motion/mot_priv.h"

int output_chan = 0;

int tpCreate(TP_STRUCT * tp, int _queueSize, TC_STRUCT * tcSpace)
{
    if (0 == tp) {
	return -1;
    }

    if (_queueSize <= 0) {
	tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
	tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
	return -1;
    }

    /* init the rest of our data */
    return tpInit(tp);
}

/*
  tpClear() is a "soft init" in the sense that the TP_STRUCT configuration
  parameters (cycleTime, vMax, and aMax) are left alone, but the queue is
  cleared, and the flags are set to an empty, ready queue. The currentPos
  is left alone, and goalPos is set to this position.

  vScale is set to vRestore.

  This function is intended to put the motion queue in the state it would
  be if all queued motions finished at the current position.
 */
int tpClear(TP_STRUCT * tp)
{
    tcqInit(&tp->queue);
    tp->goalPos.tran.x = tp->currentPos.tran.x;
    tp->goalPos.tran.y = tp->currentPos.tran.y;
    tp->goalPos.tran.z = tp->currentPos.tran.z;
    tp->goalPos.a = tp->currentPos.a;
    tp->goalPos.b = tp->currentPos.b;
    tp->goalPos.c = tp->currentPos.c;
    tp->nextId = 0;
    tp->execId = 0;
    tp->termCond = TC_TERM_COND_BLEND;
    tp->done = 1;
    tp->depth = 0;
    tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->vScale = tp->vRestore;

    return 0;
}

int tpInit(TP_STRUCT * tp)
{
    tp->cycleTime = 0.0;
    tp->vLimit = 0.0;
    tp->vScale = tp->vRestore = 1.0;
    tp->aMax = 0.0;
    tp->vMax = 0.0;
    tp->ini_maxvel = 0.0;
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    tp->currentPos.tran.x = 0.0;
    tp->currentPos.tran.y = 0.0;
    tp->currentPos.tran.z = 0.0;
    tp->currentPos.a = 0.0;
    tp->currentPos.b = 0.0;
    tp->currentPos.c = 0.0;
    
    return tpClear(tp);
}

int tpSetCycleTime(TP_STRUCT * tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
	return -1;
    }

    tp->cycleTime = secs;

    return 0;
}

// This is called before adding lines or circles, specifying
// vMax (the velocity requested by the F word) and
// ini_maxvel, the max velocity possible before meeting
// a machine constraint caused by an AXIS's max velocity.
// (the TP is allowed to go up to this high when feed 
// override >100% is requested)  These settings apply to
// subsequent moves until changed.

int tpSetVmax(TP_STRUCT * tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
	return -1;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return 0;
}

// I think this is the [TRAJ] max velocity.  This should
// be the max velocity of the TOOL TIP, not necessarily
// any particular axis.  This applies to subsequent moves
// until changed.

int tpSetVlimit(TP_STRUCT * tp, double vLimit)
{
    if (0 == tp || vLimit <= 0.0) {
	return -1;
    }

    tp->vLimit = vLimit;

    return 0;
}

// feed override, 1.0 = 100%

int tpSetVscale(TP_STRUCT * tp, double scale)
{
    TC_STRUCT *thisTc;
    int t;
    int depth;

    if (0 == tp) {
	return -1;
    }

    if (scale < 0.0) {
	/* clamp it */
	scale = 0.0;
    }

    /* record the scale factor */
    if (tp->pausing) {
	/* if we're pausing, our scale is 0 and needs to stay there so that
	   it's applied to any added motions. We'll put the requested scale
	   in the restore value so that when we resume the new scale is
	   applied. Also, don't send this down to the queued motions--
	   they're already paused */
	tp->vRestore = scale;
    } else {
	/* we're not pausing, so it goes right in and is applied to the
	   global value for subsequent moves, and also all queued moves */
	tp->vScale = scale;

	depth = tcqLen(&tp->queue);
	for (t = 0; t < depth; t++) {
	    thisTc = tcqItem(&tp->queue, t, 0);
            thisTc->vScale = scale;
	}
    }

    return 0;
}

// Set max accel

int tpSetAmax(TP_STRUCT * tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
	return -1;
    }

    tp->aMax = aMax;

    return 0;
}

/*
  tpSetId() sets the id that will be used for the next appended motions.
  nextId is incremented so that the next time a motion is appended its id
  will be one more than the previous one, modulo a signed int. If
  you want your own ids for each motion, call this before each motion
  you append and stick what you want in here.
  */
int tpSetId(TP_STRUCT * tp, int id)
{
    if (0 == tp) {
	return -1;
    }

    tp->nextId = id;

    return 0;
}

/*
  tpGetExecId() returns the id of the last motion that is currently
  executing.
  */
int tpGetExecId(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    return tp->execId;
}

/*
  tpSetTermCond(tp, cond) sets the termination condition for all subsequent
  queued moves. If cond is TC_TERM_STOP, motion comes to a stop before
  a subsequent move begins. If cond is TC_TERM_BLEND, the following move
  is begun when the current move decelerates.
  */
int tpSetTermCond(TP_STRUCT * tp, int cond)
{
    if (0 == tp) {
	return -1;
    }

    if (cond != TC_TERM_COND_STOP && cond != TC_TERM_COND_BLEND) {
	return -1;
    }

    tp->termCond = cond;

    return 0;
}

int tpGetTermCond(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    return tp->termCond;
}

int tpSetPos(TP_STRUCT * tp, EmcPose pos)
{
    if (0 == tp) {
	return -1;
    }

    tp->currentPos = pos;
    tp->goalPos = pos;

    return 0;
}

/* 'tpAddLine()' adds a straight line move to the motion
    queue (I think).  Based on how it is called, tp is
    a pointer to the queue where it should be added, and
    end is the endpoint (the start point is presumed to
    be the current location.
*/

int tpAddLine(TP_STRUCT * tp, EmcPose end)
{
    TC_STRUCT tc;
    PmLine line, line_abc;
    PmPose tran_pose, goal_tran_pose;
    PmPose abc_pose, goal_abc_pose;

    if (0 == tp) {
	return -1;
    }

    if (tp->aborting) {
	return -1;
    }

    tran_pose.tran = end.tran;
    tran_pose.rot.s = 1.0;
    tran_pose.rot.x = tran_pose.rot.y = tran_pose.rot.z = 0.0;
    goal_tran_pose.tran = tp->goalPos.tran;
    goal_tran_pose.rot.s = 1.0;
    goal_tran_pose.rot.x = goal_tran_pose.rot.y = goal_tran_pose.rot.z =
	0.0;

    abc_pose.tran.x = end.a;
    abc_pose.tran.y = end.b;
    abc_pose.tran.z = end.c;
    abc_pose.rot.s = 1.0;
    abc_pose.rot.x = abc_pose.rot.y = abc_pose.rot.z = 0.0;
    goal_abc_pose.tran.x = tp->goalPos.a;
    goal_abc_pose.tran.y = tp->goalPos.b;
    goal_abc_pose.tran.z = tp->goalPos.c;
    goal_abc_pose.rot.s = 1.0;
    goal_abc_pose.rot.x = goal_abc_pose.rot.y = goal_abc_pose.rot.z = 0.0;

    tcInit(&tc);

    pmLineInit(&line, goal_tran_pose, tran_pose);
    pmLineInit(&line_abc, goal_abc_pose, abc_pose);

    tc.cycleTime = tp->cycleTime;
    tc.tvMax = tp->vMax;
    tc.ini_maxvel = tp->ini_maxvel;
    tc.taMax = tp->aMax;
    tc.abc_vMax = tp->aMax;
    tc.abc_aMax = tp->wDotMax;
    tc.vScale = tp->vScale;
    tc.vLimit = tp->vLimit;
    tc.id = tp->nextId;
    tc.termCond = tp->termCond;

    tcSetLine(&tc, line, line_abc);
    
    // FIXME - debug only, remove later
    tc.output_chan = output_chan;
    if ( ++output_chan >= 4 ) { output_chan = 0; }
    
    if (-1 == tcqPut(&tp->queue, tc)) {
	return -1;
    }

    tp->goalPos = end;
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

int tpAddCircle(TP_STRUCT * tp, EmcPose end,
		PmCartesian center, PmCartesian normal, int turn)
{
    TC_STRUCT tc;
    PmCircle circle;
    PmLine line_abc;
    PmPose endPose, circleGoalPose;
    PmPose abc_pose, goal_abc_pose;

    if (0 == tp) {
	return -1;
    }

    if (tp->aborting) {
	return -1;
    }

    endPose.tran = end.tran;
    endPose.rot.s = 1.0;
    endPose.rot.x = endPose.rot.y = endPose.rot.z = 0.0;
    circleGoalPose.tran = tp->goalPos.tran;
    circleGoalPose.rot.s = 1.0;
    circleGoalPose.rot.x = circleGoalPose.rot.y = circleGoalPose.rot.z =
	0.0;

    tcInit(&tc);
    pmCircleInit(&circle, circleGoalPose, endPose, center, normal, turn);

    tc.cycleTime = tp->cycleTime;
    tc.tvMax = tp->vMax;
    tc.ini_maxvel = tp->ini_maxvel;
    tc.taMax = tp->aMax;
    tc.abc_vMax = tp->aMax;
    tc.abc_aMax = tp->wDotMax;
    tc.vScale = tp->vScale;
    tc.vLimit = tp->vLimit;

    abc_pose.tran.x = end.a;
    abc_pose.tran.y = end.b;
    abc_pose.tran.z = end.c;
    abc_pose.rot.s = 1.0;
    abc_pose.rot.x = abc_pose.rot.y = abc_pose.rot.z = 0.0;
    goal_abc_pose.tran.x = tp->goalPos.a;
    goal_abc_pose.tran.y = tp->goalPos.b;
    goal_abc_pose.tran.z = tp->goalPos.c;
    goal_abc_pose.rot.s = 1.0;
    goal_abc_pose.rot.x = goal_abc_pose.rot.y = goal_abc_pose.rot.z = 0.0;
    pmLineInit(&line_abc, goal_abc_pose, abc_pose);

    tcSetCircle(&tc, circle, line_abc);

    tc.id = tp->nextId;
    tc.termCond = tp->termCond;
    
    // FIXME - debug only, remove later
    tc.output_chan = output_chan;
    if ( ++output_chan >= 4 ) { output_chan = 0; }
   
    if (-1 == tcqPut(&tp->queue, tc)) {
	return -1;
    }

    tp->goalPos = end;
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

/* what the hell does this do?  It is one of the key functions in
   the whole planner, and it doesn't have any overall comments :-(
*/

int tpRunCycle(TP_STRUCT * tp)
{
    EmcPose sumPos;
    PmCartesian unitCart;
    double thisAccel = 0.0, thisVel = 0.0;
    PmCartesian thisAccelCart, thisVelCart;
    PmCartesian accelCart, velCart;
    double currentAccelMag = 0.0, currentVelMag = 0.0;
#ifdef OLD_CODE
    double dot = 0.0;
#endif

    int toRemove = 0;
    TC_STRUCT *thisTc = 0;
    int lastTcWasPureRotation = 0;
    int thisTcIsPureRotation = 0;
    int t;
    EmcPose before, after;
    double preVMax = 0.0;
    double preAMax = 0.0;

    // FIXME - debug only, remove later
    int n;
    
    if (0 == tp) {
	return -1;
    }

    sumPos.tran.x = sumPos.tran.y = sumPos.tran.z = 0.0;
    unitCart.x = unitCart.y = unitCart.z = 0.0;
    accelCart.x = accelCart.y = accelCart.z = 0.0;
    velCart.x = velCart.y = velCart.z = 0.0;
    sumPos.a = sumPos.b = sumPos.c = 0.0;

    /* correct accumulation of errors between currentPos and before */
    after.tran.x = before.tran.x = tp->currentPos.tran.x;
    after.tran.y = before.tran.y = tp->currentPos.tran.y;
    after.tran.z = before.tran.z = tp->currentPos.tran.z;
    after.a = before.a = tp->currentPos.a;
    after.b = before.b = tp->currentPos.b;
    after.c = before.c = tp->currentPos.c;

    /* run all TCs at and before this one */
    tp->activeDepth = 0;
    
    // FIXME - debug only, remove later
    for ( n = 0 ; n < 4 ; n++ ) {
	emcmot_hal_data->tc_pos[n] = 0.0;
	emcmot_hal_data->tc_vel[n] = 0.0;
	emcmot_hal_data->tc_acc[n] = 0.0;
    }
   
    /* this loops over the TC_STRUCT queue */
    for (t = 0; t < tcqLen(&tp->queue); t++) {
	lastTcWasPureRotation = thisTcIsPureRotation;
	/* get a pointer to a TC_STRUCT in the queue */
	thisTc = tcqItem(&tp->queue, t, 0);
	/* if missing or finished, plan to remove it */
	if (0 == thisTc || thisTc->tcFlag == TC_IS_DONE) {
	    if (t <= toRemove) {
		toRemove++;
	    }
	    continue;
	}
	thisTcIsPureRotation = thisTc->tmag < TP_PURE_ROTATION_EPSILON;

	if (thisTc->currentPos <= 0.0 && (tp->pausing || tp->aborting)) {
	    continue;
	}
	/* If either this move or the last move was a pure rotation reset the
	   velocity and acceleration and block any blending */
	if (lastTcWasPureRotation || thisTcIsPureRotation) {
	    velCart.x = velCart.y = velCart.z = 0.0;
	    accelCart.x = accelCart.y = accelCart.z = 0.0;
	    currentVelMag = 0.0;
	    currentAccelMag = 0.0;
	    if (thisVel > TP_VEL_EPSILON) {
		preVMax = thisTc->vMax;
	    }
	    if (thisAccel > TP_ACCEL_EPSILON
		|| thisAccel < -TP_ACCEL_EPSILON) {
		preAMax = thisTc->aMax;
	    }
	} else {
	    unitCart = tcGetUnitCart(thisTc);

	    /* The combined velocity of this move and the next one will be
	       square root(currentVelocity^2 + nextVel^2 + 2*the dot
	       product). to prevent the combined move from exceeding vMax
	       preVMax may need adjustment. tcRunCycle will subtract preVMax
	       from vMax and clamp the velocity to this value. */
	    pmCartMag(velCart, &currentVelMag);
#ifdef OLD_CODE
	    if (currentVelMag >= TP_VEL_EPSILON) {
		pmCartCartDot(velCart, unitCart, &dot);
		preVMax =
		    thisTc->vMax + dot - pmSqrt(pmSq(dot) -
						pmSq(currentVelMag) +
						pmSq(thisTc->vMax));
	    } else
#endif
            {
		preVMax = 0.0;
	    }

	    /* The combined acceleration of this move and the next one will
	       be square root(currentAcceleration^2 + nextAccel^2 + 2*the dot 
	       product). to prevent the combined move from exceeding vMax
	       preVMax may need adjustment. tcRunCycle will subtract preVMax
	       from vMax and clamp the acceleration to this value. */
	    pmCartMag(accelCart, &currentAccelMag);
	    if (currentAccelMag >= TP_ACCEL_EPSILON) {
#ifdef OLD_CODE
		pmCartCartDot(accelCart, unitCart, &dot);
		preAMax =
		    thisTc->aMax + dot - pmSqrt(pmSq(dot) -
						pmSq(currentAccelMag) +
						pmSq(thisTc->aMax));
#endif
	    } else {
		preAMax = 0.0;
	    }
	}
	/* here we calculate the contribution to motion of a single
	   line or circle, which may later be blended with others */
	before = tcGetPos(thisTc);
        thisTc->preVMax = preVMax;
        thisTc->preAMax = preAMax;
	tcRunCycle(thisTc);
	after = tcGetPos(thisTc);
	
	if (tp->activeDepth <= toRemove) {
	    tp->execId = thisTc->id;
	}
	/* calculate the move contributed by this TC */
	pmCartCartSub(after.tran, before.tran, &after.tran);
	/* add it to the running sum */
	pmCartCartAdd(sumPos.tran, after.tran, &sumPos.tran);
	sumPos.a += after.a - before.a;
	sumPos.b += after.b - before.b;
	sumPos.c += after.c - before.c;

	if (thisTc->tcFlag == TC_IS_DONE) {
	    /* this one is done-- blend in the next one */
	    if (t <= toRemove) {
		toRemove++;
	    }
	    continue;
	}

	/* this one is still active-- increment active count */
	tp->activeDepth++;

	if (thisTc->tcFlag == TC_IS_DECEL
	    && thisTc->termCond == TC_TERM_COND_BLEND) {

            preAMax = thisTc->aMax + thisTc->currentAccel;

	    /* this one is decelerating-- blend in the next one with credit
	       for this decel */
	    thisAccel = thisTc->currentAccel;
	    thisVel = thisTc->currentVel;
	    unitCart = tcGetUnitCart(thisTc);
	    
	    pmCartScalMult(unitCart, thisAccel, &thisAccelCart);
	    pmCartCartAdd(thisAccelCart, accelCart, &accelCart);
	    
	    pmCartScalMult(unitCart, thisVel, &thisVelCart);
	    pmCartCartAdd(thisVelCart, velCart, &velCart);
	    /* continue looping to get next TC */
	    continue;
	} else {
	    /* this one is either accelerating or constant-- no room for any
	       more blending */
	    preVMax = 0.0;
	    preAMax = 0.0;
	    /* exit loop, nothing else to blend */
	    break;
	}
    } /* end of loop over TC_STRUCT queue */

    // FIXME - debug only, remove later
    emcmot_hal_data->traj_active_tc = tp->activeDepth;

    if (toRemove > 0) {
	tcqRemove(&tp->queue, toRemove);
	tp->depth = tcqLen(&tp->queue);
	if (tp->depth == 0) {
	    tp->done = 1;
	    tp->activeDepth = 0;
	    tp->execId = 0;
	}
    }

    /* increment current position with sum of increments */
    pmCartCartAdd(tp->currentPos.tran, sumPos.tran, &tp->currentPos.tran);
    tp->currentPos.a += sumPos.a;
    tp->currentPos.b += sumPos.b;
    tp->currentPos.c += sumPos.c;

    // FIXME - debug only, remove later
    emcmot_hal_data->traj_pos_out = tp->currentPos.tran.x;
    emcmot_hal_data->traj_vel_out = sumPos.tran.x / tp->cycleTime;
    
    /* check for abort done */
    if (tp->aborting && (tpIsPaused(tp) || tpIsDone(tp))) {
	/* all paused and we're aborting-- clear out the TP queue */
	/* first set the motion outputs to the end values for the current
	   move */
	tcqInit(&tp->queue);
	tp->goalPos = tp->currentPos;
	tp->done = 1;
	tp->depth = 0;
	tp->activeDepth = 0;
	tp->aborting = 0;
	tp->execId = 0;
	tpResume(tp);
    }

    return 0;
}

int tpPause(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    if (!tp->pausing) {
	/* save the restore value */
	tp->vRestore = tp->vScale;

	/* apply 0 scale to queued motions */
	tpSetVscale(tp, 0);

	/* mark us pausing-- do this after the call to toSetVscale since this
	   looks at the pausing flag to decide whether to set vRestore (if
	   pausing) or vScale (if not). We want vScale to be set in the call
	   for this one */
	tp->pausing = 1;
    }

    return 0;
}

int tpResume(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    if (tp->pausing) {
	/* mark us not pausing-- this must be done before the call to
	   tpSetVscale since that function will only apply the restored scale
	   value if we're not pausing */
	tp->pausing = 0;

	/* restore scale value */
	tp->vScale = tp->vRestore;

	/* apply the restored scale value to queued motions */
	tpSetVscale(tp, tp->vScale);
    }

    return 0;
}

int tpAbort(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    if (!tp->aborting) {
	/* to abort, signal a pause and set our abort flag */
	tpPause(tp);
	tp->aborting = 1;
    }
    return 0;
}

EmcPose tpGetPos(TP_STRUCT * tp)
{
    EmcPose retval;

    if (0 == tp) {
	retval.tran.x = retval.tran.y = retval.tran.z = 0.0;
	retval.a = retval.b = retval.c = 0.0;
	return retval;
    }

    return tp->currentPos;
}

int tpIsDone(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->done;
}

/*
  tpIsPaused() returns 1 only when all active queued motions are paused.
  This is necessary so that abort clears the queue when motions have really
  stopped. If there are no queued motions, it returns 1 if the pausing
  flag is set. */
int tpIsPaused(TP_STRUCT * tp)
{
    int t;

    if (0 == tp) {
	return 0;
    }

    if (0 == tp->depth) {
	return tp->pausing;
    }

    for (t = 0; t < tp->activeDepth; t++) {
	if (!tcIsPaused(tcqItem(&tp->queue, t, 0))) {
	    return 0;
	}
    }

    return 1;
}

int tpQueueDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->activeDepth;
}

