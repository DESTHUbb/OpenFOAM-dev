/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2011-2022 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "InjectionModel.H"
#include "mathematicalConstants.H"
#include "meshTools.H"
#include "volFields.H"

using namespace Foam::constant::mathematical;

// * * * * * * * * * * * Protected Member Functions  * * * * * * * * * * * * //

template<class CloudType>
bool Foam::InjectionModel<CloudType>::prepareForNextTimeStep
(
    const scalar time,
    label& newParcels,
    scalar& newVolumeFraction
)
{
    // Initialise values
    newParcels = 0;
    newVolumeFraction = 0.0;
    bool validInjection = false;

    // Return if not started injection event
    if (time < SOI_)
    {
        timeStep0_ = time;
        return validInjection;
    }

    // Make times relative to SOI
    scalar t0 = timeStep0_ - SOI_;
    scalar t1 = time - SOI_;

    // Number of parcels to inject
    newParcels = this->parcelsToInject(t0, t1);

    // Volume of parcels to inject
    newVolumeFraction =
        this->volumeToInject(t0, t1)
       /(volumeTotal_ + rootVSmall);

    if (newVolumeFraction > 0)
    {
        if (newParcels > 0)
        {
            timeStep0_ = time;
            validInjection = true;
        }
        else
        {
            // Injection should have started, but not sufficient volume to
            // produce (at least) 1 parcel - hold value of timeStep0_
            validInjection = false;
        }
    }
    else
    {
        timeStep0_ = time;
        validInjection = false;
    }

    return validInjection;
}


template<class CloudType>
bool Foam::InjectionModel<CloudType>::findCellAtPosition
(
    const point& position,
    barycentric& coordinates,
    label& celli,
    label& tetFacei,
    label& tetPti,
    bool errorOnNotFound
)
{
    // Subroutine for finding the cell
    auto findProcAndCell = [this](const point& pos)
    {
        // Find the containing cell
        label celli = this->owner().mesh().findCell(pos);

        // Synchronise so only a single processor finds this position
        label proci = celli >= 0 ? Pstream::myProcNo() : -1;
        reduce(proci, maxOp<label>());
        if (proci != Pstream::myProcNo())
        {
            celli = -1;
        }

        return labelPair(proci, celli);
    };

    point pos = position;

    // Try and find the cell at the given position
    const labelPair procAndCelli = findProcAndCell(pos);
    label proci = procAndCelli.first();
    celli = procAndCelli.second();

    // Didn't find it. The point may be awkwardly on an edge or face. Try
    // again, but move the point into its nearest cell a little bit.
    if (proci == -1)
    {
        pos += small*(this->owner().mesh().C()[celli] - pos);
        const labelPair procAndCelli = findProcAndCell(pos);
        proci = procAndCelli.first();
        celli = procAndCelli.second();
    }

    // Didn't find it. Error or return false.
    if (proci == -1)
    {
        if (errorOnNotFound)
        {
            FatalErrorInFunction
                << "Cannot find parcel injection cell. "
                << "Parcel position = " << position << nl
                << exit(FatalError);
        }

        return false;
    }

    // Found it. Construct the barycentric coordinates.
    if (proci == Pstream::myProcNo())
    {
        particle p(this->owner().mesh(), pos, celli);
        coordinates = p.coordinates();
        celli = p.cell();
        tetFacei = p.tetFace();
        tetPti = p.tetPt();
    }

    return true;
}


template<class CloudType>
void Foam::InjectionModel<CloudType>::constrainPosition
(
    typename CloudType::parcelType::trackingData& td,
    typename CloudType::parcelType& parcel
)
{
    const vector d = parcel.deviationFromMeshCentre(td.mesh);

    if (d == vector::zero)
    {
        return;
    }

    const label facei = parcel.face();

    // If the parcel is not on a face, then just track it to the mesh centre
    if (facei == -1)
    {
        parcel.track(td.mesh, - d, 0);
    }

    // If the parcel is on a face, then track in two steps, going slightly into
    // the current cell. This prevents a boundary hit from ending the track
    // prematurely.
    if (facei != -1)
    {
        const vector pc =
            td.mesh.cellCentres()[parcel.cell()] - parcel.position(td.mesh);

        parcel.track(td.mesh, - d/2 + rootSmall*pc, 0);
        parcel.track(td.mesh, - d/2 - rootSmall*pc, 0);
    }

    // Restore any face-association that got changed during tracking
    parcel.face() = facei;
}


template<class CloudType>
Foam::scalar Foam::InjectionModel<CloudType>::setNumberOfParticles
(
    const label parcels,
    const scalar volumeFraction,
    const scalar diameter,
    const scalar rho
)
{
    scalar nP = 0.0;
    switch (parcelBasis_)
    {
        case pbMass:
        {
            scalar volumep = pi/6.0*pow3(diameter);
            scalar volumeTot = massTotal_/rho;

            nP = volumeFraction*volumeTot/(parcels*volumep);
            break;
        }
        case pbNumber:
        {
            nP = massTotal_/(rho*volumeTotal_);
            break;
        }
        case pbFixed:
        {
            nP = nParticleFixed_;
            break;
        }
        default:
        {
            nP = 0.0;
            FatalErrorInFunction
                << "Unknown parcelBasis type" << nl
                << exit(FatalError);
        }
    }

    return nP;
}


template<class CloudType>
void Foam::InjectionModel<CloudType>::postInjectCheck
(
    const label parcelsAdded,
    const scalar massAdded
)
{
    const label allParcelsAdded = returnReduce(parcelsAdded, sumOp<label>());

    if (allParcelsAdded > 0)
    {
        Info<< nl
            << "Cloud: " << this->owner().name()
            << " injector: " << this->modelName() << nl
            << "    Added " << allParcelsAdded << " new parcels" << nl << endl;
    }

    // Increment total number of parcels added
    parcelsAddedTotal_ += allParcelsAdded;

    // Increment total mass injected
    massInjected_ += returnReduce(massAdded, sumOp<scalar>());

    // Update time for start of next injection
    time0_ = this->owner().db().time().value();

    // Increment number of injections
    nInjections_++;
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

template<class CloudType>
Foam::InjectionModel<CloudType>::InjectionModel(CloudType& owner)
:
    CloudSubModelBase<CloudType>(owner),
    SOI_(0.0),
    volumeTotal_(0.0),
    massTotal_(0.0),
    massFlowRate_(owner.db().time(), "massFlowRate"),
    massInjected_(this->template getModelProperty<scalar>("massInjected")),
    nInjections_(this->template getModelProperty<label>("nInjections")),
    parcelsAddedTotal_
    (
        this->template getModelProperty<scalar>("parcelsAddedTotal")
    ),
    parcelBasis_(pbNumber),
    nParticleFixed_(0.0),
    time0_(0.0),
    timeStep0_(this->template getModelProperty<scalar>("timeStep0"))
{}


template<class CloudType>
Foam::InjectionModel<CloudType>::InjectionModel
(
    const dictionary& dict,
    CloudType& owner,
    const word& modelName,
    const word& modelType
)
:
    CloudSubModelBase<CloudType>(modelName, owner, dict, typeName, modelType),
    SOI_(0.0),
    volumeTotal_(0.0),
    massTotal_(0.0),
    massFlowRate_(owner.db().time(), "massFlowRate"),
    massInjected_(this->template getModelProperty<scalar>("massInjected")),
    nInjections_(this->template getModelProperty<scalar>("nInjections")),
    parcelsAddedTotal_
    (
        this->template getModelProperty<scalar>("parcelsAddedTotal")
    ),
    parcelBasis_(pbNumber),
    nParticleFixed_(0.0),
    time0_(owner.db().time().value()),
    timeStep0_(this->template getModelProperty<scalar>("timeStep0"))
{
    // Provide some info
    // - also serves to initialise mesh dimensions - needed for parallel runs
    //   due to lazy evaluation of valid mesh dimensions
    Info<< "    Constructing " << owner.mesh().nGeometricD() << "-D injection"
        << endl;

    if (owner.solution().transient())
    {
        this->coeffDict().lookup("massTotal") >> massTotal_;
        this->coeffDict().lookup("SOI") >> SOI_;
        SOI_ = owner.db().time().userTimeToTime(SOI_);
    }
    else
    {
        massFlowRate_.reset(this->coeffDict());
        massTotal_ = massFlowRate_.value(owner.db().time().value());
    }

    const word parcelBasisType = this->coeffDict().lookup("parcelBasisType");

    if (parcelBasisType == "mass")
    {
        parcelBasis_ = pbMass;
    }
    else if (parcelBasisType == "number")
    {
        parcelBasis_ = pbNumber;
    }
    else if (parcelBasisType == "fixed")
    {
        parcelBasis_ = pbFixed;

        Info<< "    Choosing nParticle to be a fixed value, massTotal "
            << "variable now does not determine anything."
            << endl;

        nParticleFixed_ =
            this->coeffDict().template lookup<scalar>("nParticle");
    }
    else
    {
        FatalErrorInFunction
            << "parcelBasisType must be either 'number', 'mass' or 'fixed'"
            << nl << exit(FatalError);
    }
}


template<class CloudType>
Foam::InjectionModel<CloudType>::InjectionModel
(
    const InjectionModel<CloudType>& im
)
:
    CloudSubModelBase<CloudType>(im),
    SOI_(im.SOI_),
    volumeTotal_(im.volumeTotal_),
    massTotal_(im.massTotal_),
    massFlowRate_(im.massFlowRate_),
    massInjected_(im.massInjected_),
    nInjections_(im.nInjections_),
    parcelsAddedTotal_(im.parcelsAddedTotal_),
    parcelBasis_(im.parcelBasis_),
    nParticleFixed_(im.nParticleFixed_),
    time0_(im.time0_),
    timeStep0_(im.timeStep0_)
{}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

template<class CloudType>
Foam::InjectionModel<CloudType>::~InjectionModel()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

template<class CloudType>
void Foam::InjectionModel<CloudType>::topoChange()
{}


template<class CloudType>
Foam::scalar Foam::InjectionModel<CloudType>::averageParcelMass()
{
    label nTotal = 0.0;
    if (this->owner().solution().transient())
    {
        nTotal = parcelsToInject(0.0, timeEnd() - timeStart());
    }
    else
    {
        nTotal = parcelsToInject(0.0, 1.0);
    }

    return massTotal_/nTotal;
}


template<class CloudType>
template<class TrackCloudType>
void Foam::InjectionModel<CloudType>::inject
(
    TrackCloudType& cloud,
    typename CloudType::parcelType::trackingData& td
)
{
    const polyMesh& mesh = this->owner().mesh();

    const scalar time = this->owner().db().time().value();

    // Prepare for next time step
    label parcelsAdded = 0;
    scalar massAdded = 0.0;
    label newParcels = 0;
    scalar newVolumeFraction = 0.0;

    if (prepareForNextTimeStep(time, newParcels, newVolumeFraction))
    {
        // Duration of injection period during this timestep
        const scalar deltaT =
            max(0.0, min(td.trackTime(), min(time - SOI_, timeEnd() - time0_)));

        // Pad injection time if injection starts during this timestep
        const scalar padTime = max(0.0, SOI_ - time0_);

        // Introduce new parcels linearly across carrier phase timestep
        for (label parcelI = 0; parcelI < newParcels; parcelI++)
        {
            if (validInjection(parcelI))
            {
                // Calculate the pseudo time of injection for parcel 'parcelI'
                scalar timeInj = time0_ + padTime + deltaT*parcelI/newParcels;

                // Determine the injection coordinates and owner cell,
                // tetFace and tetPt
                barycentric coordinates = barycentric::uniform(NaN);
                label celli = -1, tetFacei = -1, tetPti = -1, facei = -1;
                setPositionAndCell
                (
                    parcelI,
                    newParcels,
                    timeInj,
                    coordinates,
                    celli,
                    tetFacei,
                    tetPti,
                    facei
                );

                if (celli > -1)
                {
                    // Lagrangian timestep
                    const scalar dt = timeInj - time0_;

                    // Create a new parcel
                    parcelType* pPtr =
                        new parcelType
                        (
                            mesh,
                            coordinates,
                            celli,
                            tetFacei,
                            tetPti,
                            facei
                        );

                    // Correct the position for reduced-dimension cases
                    constrainPosition(td, *pPtr);

                    // Check/set new parcel thermo properties
                    cloud.setParcelThermoProperties(*pPtr);

                    // Assign new parcel properties in injection model
                    setProperties(parcelI, newParcels, timeInj, *pPtr);

                    // Check/set new parcel injection properties
                    cloud.checkParcelProperties(*pPtr, fullyDescribed());

                    // Apply correction to velocity for 2-D cases
                    meshTools::constrainDirection
                    (
                        mesh,
                        mesh.solutionD(),
                        pPtr->U()
                    );

                    // Number of particles per parcel
                    pPtr->nParticle() =
                        setNumberOfParticles
                        (
                            newParcels,
                            newVolumeFraction,
                            pPtr->d(),
                            pPtr->rho()
                        );

                    // Modify the step fraction so that the particles are
                    // injected continually through the time-step
                    pPtr->stepFraction() = dt/td.trackTime();

                    // Add the new parcel
                    parcelsAdded ++;
                    massAdded += pPtr->nParticle()*pPtr->mass();
                    cloud.addParticle(pPtr);
                }
            }
        }
    }

    postInjectCheck(parcelsAdded, massAdded);
}


template<class CloudType>
template<class TrackCloudType>
void Foam::InjectionModel<CloudType>::injectSteadyState
(
    TrackCloudType& cloud,
    typename CloudType::parcelType::trackingData& td
)
{
    const polyMesh& mesh = this->owner().mesh();

    massTotal_ = massFlowRate_.value(mesh.time().value());

    // Reset counters
    time0_ = 0.0;
    label parcelsAdded = 0;
    scalar massAdded = 0.0;

    // Set number of new parcels to inject based on first second of injection
    label newParcels = parcelsToInject(0.0, 1.0);

    // Inject new parcels
    for (label parcelI = 0; parcelI < newParcels; parcelI++)
    {
        // Volume to inject is split equally amongst all parcel streams
        scalar newVolumeFraction = 1.0/scalar(newParcels);

        // Determine the injection coordinates and owner cell,
        // tetFace and tetPt
        barycentric coordinates = barycentric::uniform(NaN);
        label celli = -1, tetFacei = -1, tetPti = -1, facei = -1;
        setPositionAndCell
        (
            parcelI,
            newParcels,
            0,
            coordinates,
            celli,
            tetFacei,
            tetPti,
            facei
        );

        if (celli > -1)
        {
            // Create a new parcel
            parcelType* pPtr =
                new parcelType
                (
                    mesh,
                    coordinates,
                    celli,
                    tetFacei,
                    tetPti,
                    facei
                );

            // Correct the position for reduced-dimension cases
            constrainPosition(td, *pPtr);

            // Check/set new parcel thermo properties
            cloud.setParcelThermoProperties(*pPtr);

            // Assign new parcel properties in injection model
            setProperties(parcelI, newParcels, 0.0, *pPtr);

            // Check/set new parcel injection properties
            cloud.checkParcelProperties(*pPtr, fullyDescribed());

            // Apply correction to velocity for 2-D cases
            meshTools::constrainDirection(mesh, mesh.solutionD(), pPtr->U());

            // Number of particles per parcel
            pPtr->nParticle() =
                setNumberOfParticles
                (
                    1,
                    newVolumeFraction,
                    pPtr->d(),
                    pPtr->rho()
                );

            // Initial step fraction
            pPtr->stepFraction() = 0;

            // Add the new parcel
            parcelsAdded ++;
            massAdded += pPtr->nParticle()*pPtr->mass();
            cloud.addParticle(pPtr);
        }
    }

    postInjectCheck(parcelsAdded, massAdded);
}


template<class CloudType>
void Foam::InjectionModel<CloudType>::info(Ostream& os)
{
    os  << "    " << this->modelName() << ":" << nl
        << "        number of parcels added     = " << parcelsAddedTotal_ << nl
        << "        mass introduced             = " << massInjected_ << nl;

    if (this->writeTime())
    {
        this->setModelProperty("massInjected", massInjected_);
        this->setModelProperty("nInjections", nInjections_);
        this->setModelProperty("parcelsAddedTotal", parcelsAddedTotal_);
        this->setModelProperty("timeStep0", timeStep0_);
    }
}


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

#include "InjectionModelNew.C"

// ************************************************************************* //
