/* -------------------------------------------------------------------------- *
 *                      SimTK Core: SimTK Simmath(tm)                         *
 * -------------------------------------------------------------------------- *
 * This is part of the SimTK Core biosimulation toolkit originating from      *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2007 Stanford University and the Authors.           *
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

#include "SimTKcommon.h"
#include "CPodesIntegratorRep.h"

using namespace SimTK;

/**
 * This class implements the abstract CPodesSystem interface understood by our C++ interface to CPodes.
 */
class CPodesIntegratorRep::CPodesSystemImpl : public CPodesSystem {
public:
    CPodesSystemImpl(CPodesIntegratorRep& integ, const System& system) : integ(integ), system(system) {
    }

    // Calculate ydot = f(t,y).
    int explicitODE(Real t, const Vector& y, Vector& ydot) const {
        State advanced = integ.updAdvancedState();
        advanced.updY() = y;
        advanced.updTime() = t;
        try { 
            system.realize(advanced, Stage::Acceleration); 
        }
        catch(...) { return CPodes::RecoverableError; } // assume recoverable
        ydot = advanced.getYDot();
        return CPodes::Success;
    }

    // Calculate yerr = c(t,y).
    int constraint(Real t, const Vector& y, Vector& yerr) const {
        State advanced = integ.updAdvancedState();
        advanced.updY() = y;
        advanced.updTime() = t;
        try { 
            system.realize(advanced, Stage::Velocity); 
        }
        catch(...) { return CPodes::RecoverableError; } // assume recoverable
        yerr = advanced.getYErr();
        return CPodes::Success;
    }

    // Given a state (t,y) not on the constraint manifold, return ycorr
    // such that (t,y+ycorr+eps) is on the manifold, with ||eps||_wrms <= epsProj. 
    // 'err' passed in as the integrator's current error estimate for state y;
    // optionally project it to eliminate the portion normal to the manifold.
    int project(Real t, const Vector& y, Vector& ycorr, Real epsProj, Vector& err) const {
        State advanced = integ.updAdvancedState();
        advanced.updY() = y;
        advanced.updTime() = t;
        try {
            const Real tol = integ.getConstraintToleranceInUse();
            Vector yUnitWeights, unitTolerances;
            system.realize(advanced, Stage::Position);
            system.calcYUnitWeights(advanced, yUnitWeights);
            system.calcYErrUnitTolerances(advanced, unitTolerances);
            system.project(advanced, tol, yUnitWeights, unitTolerances, err);
        }
        catch (...) { return CPodes::RecoverableError; } // assume recoverable
        ycorr = advanced.getY()-y;
        return CPodes::Success;
    }
    
    /**
     * Calculate the event trigger functions.
     */
    int root(Real t, const Vector& y, const Vector& yp, Vector& gout) const {
        State advanced = integ.updAdvancedState();
        advanced.updY() = y;
        advanced.updTime() = t;
        try { 
            system.realize(advanced, Stage::Acceleration); 
        }
        catch(...) { return CPodes::RecoverableError; } // assume recoverable
        gout = advanced.getEvents();
        return CPodes::Success;
    }
private:
    CPodesIntegratorRep& integ;
    const System& system;
};

void CPodesIntegratorRep::init(CPodes::LinearMultistepMethod method, CPodes::NonlinearSystemIterationType iterationType) {
    cpodes = new CPodes(CPodes::ExplicitODE, method, iterationType);
    cps = new CPodesSystemImpl(*this, getSystem());
    initialized = false;
    useCpodesProjection = false;
}

CPodesIntegratorRep::CPodesIntegratorRep(Integrator* handle, const System& sys, CPodes::LinearMultistepMethod method) : IntegratorRep(handle, sys), method(method) {
    init(method, method == CPodes::Adams ? CPodes::Functional : CPodes::Newton);
}

CPodesIntegratorRep::CPodesIntegratorRep(Integrator* handle, const System& sys, CPodes::LinearMultistepMethod method, CPodes::NonlinearSystemIterationType iterationType) : IntegratorRep(handle, sys), method(method) {
    init(method, iterationType);
}

CPodesIntegratorRep::~CPodesIntegratorRep() {
    delete cpodes;
    delete cps;
}

void CPodesIntegratorRep::methodInitialize(const State& state) {
    if (state.getSystemStage() < Stage::Model)
        reconstructForNewModel();
    initializeIntegrationParameters();
    initialized = true;
    pendingReturnCode = -1;
    previousStartTime = 0.0;
    getSystem().realize(state, Stage::Velocity);
    const int ny = state.getY().size();
    const int nc = state.getNYErr();
    Vector ydot(ny);
    if (cps->explicitODE(state.getTime(), Vector(state.getY()), ydot) != CPodes::Success) {
        SimTK_THROW1(Integrator::InitializationFailed, "Failed to calculate ydot");
    }
    int retval;
    if ((retval=cpodes->init(*cps, state.getTime(), Vector(state.getY()), ydot, CPodes::ScalarScalar, relTol, &absTol)) != CPodes::Success) {
        printf("init() returned %d\n", retval);
        SimTK_THROW1(Integrator::InitializationFailed, "init() failed");
    }
    cpodes->lapackDense(ny);
    cpodes->setNonlinConvCoef(0.01); // TODO (default is 0.1)
    if (useCpodesProjection) {
        cpodes->projInit(CPodes::ErrorNorm, CPodes::Nonlinear, getAccuracyInUse()*getConstraintWeightsInUse());
        cpodes->lapackDenseProj(nc, ny, CPodes::ProjectWithLU);
    }
    else {
        cpodes->projDefine();
    }
    cpodes->rootInit(state.getNEvents());
}

void CPodesIntegratorRep::methodReinitialize(Stage stage, bool shouldTerminate) {
    if (stage < Stage::Report) {
        pendingReturnCode = -1;
        State state = getAdvancedState();
        getSystem().realize(state, Stage::Acceleration);
        cpodes->reInit(*cps, state.getTime(), Vector(state.getY()), Vector(state.getYDot()), CPodes::ScalarScalar, relTol, &absTol);
    }
}

void CPodesIntegratorRep::initializeIntegrationParameters() {
    if (userInitStepSize != -1)
        cpodes->setInitStep(userInitStepSize);
    if (userMinStepSize != -1)
        cpodes->setMinStep(userMinStepSize);
    if (userMaxStepSize != -1)
        cpodes->setMaxStep(userMaxStepSize);
    if (userFinalTime != -1.) 
        cpodes->setStopTime(userFinalTime);
    if (userInternalStepLimit != -1) 
        cpodes->setMaxNumSteps(userInternalStepLimit);
    if (userProjectEveryStep != -1)
        if (userProjectEveryStep==1)
            cpodes->setProjFrequency(1); // every step
}

void CPodesIntegratorRep::reconstructForNewModel() {
    initialized = false;
    delete cpodes;
    cpodes = new CPodes(CPodes::ExplicitODE, CPodes::BDF, CPodes::Newton);
}

// Create an interpolated state at time t, which is between tPrev and tCurrent.
// If we haven't yet delivered an interpolated state in this interval, we have
// to initialize its discrete part from the advanced state.
void CPodesIntegratorRep::createInterpolatedState(Real t) {
    const State& advanced = getAdvancedState();
    State&       interp   = updInterpolatedState();
    interp = advanced; // pick up discrete stuff.
    Vector yout(advanced.getY().size());
    cpodes->getDky(t, 0, yout);
    interp.updY() = yout;
    interp.updTime() = t;
}

Integrator::SuccessfulStepStatus CPodesIntegratorRep::stepTo(Real reportTime, Real scheduledEventTime) {
    assert(initialized);
    assert(reportTime >= getPreviousTime());
    assert(scheduledEventTime<=0 || scheduledEventTime >= getState().getTime());
    
    // If this is the start of a continuous interval, return immediately so
    // the current state will be seen as part of the trajectory.

    if (startOfContinuousInterval) {
        startOfContinuousInterval = false;
        return Integrator::StartOfContinuousInterval;
    }
    if (scheduledEventTime <= 0) 
        scheduledEventTime = Infinity;
    Real tMax = std::min(reportTime, scheduledEventTime);
    CPodes::StepMode mode;
    if (userFinalTime != -1) {
        if (userReturnEveryInternalStep == 1)
            mode = CPodes::OneStepTstop;
        else
            mode = CPodes::NormalTstop;
    }
    else {
        if (userReturnEveryInternalStep == 1)
            mode = CPodes::OneStep;
        else
            mode = CPodes::Normal;
    }
    
    // Ask CPodes to perform the integration.
    
    while (true) {
        Real tret;
        int res;
        if (pendingReturnCode == -1) {
            previousStartTime = getAdvancedTime();
            Vector yout(getAdvancedState().getY().size());
            Vector ypout(getAdvancedState().getY().size()); // ignored
            res = cpodes->step(tMax, &tret, yout, ypout, mode);
            updAdvancedState().updY() = yout;
            previousTimeReturned = tret;
        }
        else {
            
            // The last time returned was an event or report time.  The integrator has already
            // gone beyond that time, so reset everything to how it was after the last call to
            // cpodes->step().
            
            res = pendingReturnCode;
            tret = previousTimeReturned;
            if (savedY.size() > 0)
                updAdvancedState().updY() = savedY;
            pendingReturnCode = -1;
        }
        updAdvancedState().updTime() = tret;
        realizeStateDerivatives(getAdvancedState());
        
        // Check for integration errors.
        
        if (res == CPodes::TooMuchWork) {
            
            // The maximum number of steps was reached.
            
            setStepCommunicationStatus(IntegratorRep::StepHasBeenReturnedNoEvent);
            return Integrator::ReachedStepLimit;
        }
        if (res < 0) {
            
            // An error of some sort occurred.
            
            SimTK_THROW2(Integrator::StepFailed, getAdvancedState().getTime(), "CPodes::step() returned an error");
        }
        
        // If necessary, generate an interpolated state.
        
        if (tret > tMax) {
            setUseInterpolatedState(true);
            createInterpolatedState(tMax);
            realizeStateDerivatives(getInterpolatedState());
        }
        else
            setUseInterpolatedState(false);
        
        // Determine the correct return code.
        
        if (tret >= reportTime && reportTime <= scheduledEventTime) {
            
            // We reached the scheduled report time.
            
            savedY.resize(0);
            pendingReturnCode = res;
            setStepCommunicationStatus(IntegratorRep::StepHasBeenReturnedNoEvent);
            return Integrator::ReachedReportTime;
        }
        if (tret >= scheduledEventTime) {
            
            // We reached a scheduled event time.
            
            if (tret > scheduledEventTime) {
                
                // Back up the advanced state to the event time.
                
                savedY = getAdvancedState().getY();
                updAdvancedState().updY() = getInterpolatedState().getY();
                updAdvancedState().updTime() = scheduledEventTime;
                realizeStateDerivatives(getAdvancedState());
            }
            pendingReturnCode = res;
            setStepCommunicationStatus(IntegratorRep::StepHasBeenReturnedWithEvent);
            return Integrator::ReachedScheduledEvent;
        }
        if (res == CPodes::TstopReturn) {
            
            // The specified final time was reached.
    
            setStepCommunicationStatus(IntegratorRep::FinalTimeHasBeenReturned);
            terminationReason = Integrator::ReachedFinalTime;
            return Integrator::EndOfSimulation;
        }
        if (res == CPodes::RootReturn) {
            
            // An event was triggered.
            
            std::vector<int> eventIds;
            int nevents = getAdvancedState().getNEvents();
            int* eventFlags = new int[nevents];
            cpodes->getRootInfo(eventFlags);
            for (int i = 0; i < nevents; ++i)
                if (eventFlags[i] == 1)
                    eventIds.push_back(i);
            delete[] eventFlags;
            findEventIds(eventIds);
            std::vector<Real> eventTimes(eventIds.size());
            std::vector<EventStatus::EventTrigger> eventTransitions(eventIds.size());
            for (int i = 0; i < (int)eventIds.size(); ++i) {
                eventTimes[i] = tret;
                eventTransitions[i] = EventStatus::AnySignChange;
            }
            setTriggeredEvents(previousStartTime, tret, eventIds, eventTimes, eventTransitions);
            setStepCommunicationStatus(IntegratorRep::StepHasBeenReturnedWithEvent);
            return Integrator::ReachedEventTrigger;
        }
        if (userReturnEveryInternalStep == 1) {
            
            // The user asked to be notified of every internal step.
            
            setStepCommunicationStatus(IntegratorRep::StepHasBeenReturnedNoEvent);
            return Integrator::TimeHasAdvanced;
        }
    }
}

Real CPodesIntegratorRep::getActualInitialStepSizeTaken() const {
    assert(initialized);
    Real size;
    cpodes->getActualInitStep(&size);
    return size;
}

Real CPodesIntegratorRep::getPreviousStepSizeTaken() const {
    assert(initialized);
    Real size;
    cpodes->getLastStep(&size);
    return size;
}

Real CPodesIntegratorRep::getPredictedNextStepSize() const {
    assert(initialized);
    Real size;
    cpodes->getCurrentStep(&size);
    return size;
}

long CPodesIntegratorRep::getNStepsAttempted() const {
    assert(initialized);
    return statsStepsAttempted;
}

long CPodesIntegratorRep::getNStepsTaken() const {
    assert(initialized);
    return statsStepsTaken;
}

long CPodesIntegratorRep::getNErrorTestFailures() const {
    assert(initialized);
    return statsErrorTestFailures;
}

void CPodesIntegratorRep::resetMethodStatistics() {
    statsStepsAttempted = 0;
    statsStepsTaken = 0;
    statsErrorTestFailures = 0;
}

const char* CPodesIntegratorRep::getMethodName() const {
    return (method == CPodes::BDF ? "CPodesBDF" : "CPodesAdams");
}

int CPodesIntegratorRep::getMethodMinOrder() const {
    return 1;
}

int CPodesIntegratorRep::getMethodMaxOrder() const {
    return (method == CPodes::BDF ? 5 : 12);
}

bool CPodesIntegratorRep::methodHasErrorControl() const {
    return true;
}

void CPodesIntegratorRep::setUseCPodesProjection() {
    SimTK_APIARGCHECK_ALWAYS(!initialized, "CPodesIntegrator", "setUseCPodesProjection",
        "This method may not be invoked after the integrator has been initialized.");
    useCpodesProjection = true;
}