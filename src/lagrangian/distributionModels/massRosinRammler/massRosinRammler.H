/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2016-2023 OpenFOAM Foundation
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

Class
    Foam::distributionModels::massRosinRammler

Description
    Corrected form of the Rosin-Rammler distribution which applies
    coefficients relating to a number (q0) distribution of particle
    diameters to parcels of fixed mass.

    Parcels of fixed mass are specified by the following setting:
    \verbatim
        parcelBasisType mass;
    \endverbatim

    If coefficients for a mass/volume (q3) distribution are given
    instead, then the standard Rosin-Rammler distribution can be applied
    to parcels of fixed mass.

    See equation 10 in reference:
    \verbatim
        Yoon, S. S., Hewson, J. C., DesJardin, P. E., Glaze, D. J.,
        Black, A. R., & Skaggs, R. R. (2004).
        Numerical modeling and experimental measurements of a high speed
        solid-cone water spray for use in fire suppression applications.
        International Journal of Multiphase Flow, 30(11), 1369-1388.
    \endverbatim

SourceFiles
    massRosinRammler.C

\*---------------------------------------------------------------------------*/

#ifndef massRosinRammler_H
#define massRosinRammler_H

#include "distributionModel.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{
namespace distributionModels
{

/*---------------------------------------------------------------------------*\
                      Class massRosinRammler Declaration
\*---------------------------------------------------------------------------*/

class massRosinRammler
:
    public distributionModel
{
    // Private Data

        //- Distribution minimum
        scalar minValue_;

        //- Distribution maximum
        scalar maxValue_;

        //- Characteristic droplet size
        scalar d_;

        //- Empirical dimensionless constant to specify the distribution width,
        //  sometimes referred to as the dispersion coefficient
        scalar n_;


public:

    //- Runtime type information
    TypeName("massRosinRammler");


    // Constructors

        //- Construct from components
        massRosinRammler(const dictionary& dict, Random& rndGen);

        //- Construct copy
        massRosinRammler(const massRosinRammler& p);

        //- Construct and return a clone
        virtual autoPtr<distributionModel> clone() const
        {
            return autoPtr<distributionModel>(new massRosinRammler(*this));
        }


    //- Destructor
    virtual ~massRosinRammler();


    // Member Functions

        //- Sample the distributionModel
        virtual scalar sample() const;

        //- Return the minimum value
        virtual scalar minValue() const;

        //- Return the maximum value
        virtual scalar maxValue() const;

        //- Return the mean value
        virtual scalar meanValue() const;
};


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace distributionModels
} // End namespace Foam

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

#endif

// ************************************************************************* //
