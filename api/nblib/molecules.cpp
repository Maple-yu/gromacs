/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2020, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief
 * Implements nblib Molecule
 *
 * \author Victor Holanda <victor.holanda@cscs.ch>
 * \author Joe Jordan <ejjordan@kth.se>
 * \author Prashanth Kanduri <kanduri@cscs.ch>
 * \author Sebastian Keller <keller@cscs.ch>
 * \author Artem Zhmurov <zhmurov@gmail.com>
 */
#include <algorithm>
#include <tuple>

#include "nblib/exception.h"
#include "nblib/molecules.h"
#include "nblib/particletype.h"
#include "nblib/util/internal.h"

namespace nblib
{


Molecule::Molecule(MoleculeName moleculeName) : name_(std::move(moleculeName)) {}

MoleculeName Molecule::name() const
{
    return name_;
}

Molecule& Molecule::addParticle(const ParticleName& particleName,
                                const ResidueName&  residueName,
                                const Charge&       charge,
                                ParticleType const& particleType)
{
    auto found = particleTypes_.find(particleType.name());
    if (found == particleTypes_.end())
    {
        particleTypes_.insert(std::make_pair(particleType.name(), particleType));
    }
    else
    {
        if (!(found->second == particleType))
        {
            throw InputException(
                    "Differing ParticleTypes with identical names encountered in the same "
                    "molecule.");
        }
    }

    particles_.emplace_back(ParticleData{ particleName, residueName, particleType.name(), charge });

    // Add self exclusion. We just added the particle, so we know its index and that the exclusion doesn't exist yet
    std::size_t id = particles_.size() - 1;
    exclusions_.emplace_back(id, id);

    return *this;
}

Molecule& Molecule::addParticle(const ParticleName& particleName,
                                const ResidueName&  residueName,
                                ParticleType const& particleType)
{
    addParticle(particleName, residueName, Charge(0), particleType);

    return *this;
}

Molecule& Molecule::addParticle(const ParticleName& particleName,
                                const Charge&       charge,
                                ParticleType const& particleType)
{
    addParticle(particleName, ResidueName(name_), charge, particleType);

    return *this;
}

Molecule& Molecule::addParticle(const ParticleName& particleName, const ParticleType& particleType)
{
    addParticle(particleName, ResidueName(name_), Charge(0), particleType);

    return *this;
}

//! Two-particle interactions such as bonds and LJ1-4
template<class Interaction>
void Molecule::addInteraction(const ParticleName& particleNameI,
                              const ResidueName&  residueNameI,
                              const ParticleName& particleNameJ,
                              const ResidueName&  residueNameJ,
                              const Interaction&  interaction)
{
    if (particleNameI == particleNameJ and residueNameI == residueNameJ)
    {
        throw InputException(std::string("Cannot add interaction of particle ")
                             + particleNameI.value() + " with itself in molecule " + name_.value());
    }

    auto& interactionContainer = pickType<Interaction>(interactionData_);
    interactionContainer.interactions_.emplace_back(particleNameI, residueNameI, particleNameJ, residueNameJ);
    interactionContainer.interactionTypes_.push_back(interaction);
}

//! \cond DO_NOT_DOCUMENT
#define ADD_INTERACTION_INSTANTIATE_TEMPLATE(x)                               \
    template void Molecule::addInteraction(const ParticleName& particleNameI, \
                                           const ResidueName&  residueNameI,  \
                                           const ParticleName& particleNameJ, \
                                           const ResidueName&  residueNameJ,  \
                                           const x&            interaction);
MAP(ADD_INTERACTION_INSTANTIATE_TEMPLATE, SUPPORTED_TWO_CENTER_TYPES)
#undef ADD_INTERACTION_INSTANTIATE_TEMPLATE
//! \endcond

// add interactions with default residue name
template<class Interaction>
void Molecule::addInteraction(const ParticleName& particleNameI,
                              const ParticleName& particleNameJ,
                              const Interaction&  interaction)
{
    addInteraction(particleNameI, ResidueName(name_), particleNameJ, ResidueName(name_), interaction);
}

//! \cond DO_NOT_DOCUMENT
#define ADD_INTERACTION_INSTANTIATE_TEMPLATE(x) \
    template void Molecule::addInteraction(     \
            const ParticleName& particleNameI, const ParticleName& particleNameJ, const x& interaction);
MAP(ADD_INTERACTION_INSTANTIATE_TEMPLATE, SUPPORTED_TWO_CENTER_TYPES)
#undef ADD_INTERACTION_INSTANTIATE_TEMPLATE

//! 3-particle interactions such as angles
template<class Interaction>
void Molecule::addInteraction(const ParticleName& particleNameI,
                              const ResidueName&  residueNameI,
                              const ParticleName& particleNameJ,
                              const ResidueName&  residueNameJ,
                              const ParticleName& particleNameK,
                              const ResidueName&  residueNameK,
                              const Interaction&  interaction)
{
    if (particleNameI == particleNameJ and particleNameJ == particleNameK)
    {
        throw InputException(std::string("Cannot add interaction of particle ")
                             + particleNameI.value() + " with itself in molecule " + name_.value());
    }

    auto& interactionContainer = pickType<Interaction>(interactionData_);
    interactionContainer.interactions_.emplace_back(
            particleNameI, residueNameI, particleNameJ, residueNameJ, particleNameK, residueNameK);
    interactionContainer.interactionTypes_.push_back(interaction);
}

#define ADD_INTERACTION_INSTANTIATE_TEMPLATE(x)                               \
    template void Molecule::addInteraction(const ParticleName& particleNameI, \
                                           const ResidueName&  residueNameI,  \
                                           const ParticleName& particleNameJ, \
                                           const ResidueName&  residueNameJ,  \
                                           const ParticleName& particleNameK, \
                                           const ResidueName&  residueNameK,  \
                                           const x&            interaction);
MAP(ADD_INTERACTION_INSTANTIATE_TEMPLATE, SUPPORTED_THREE_CENTER_TYPES)
#undef ADD_INTERACTION_INSTANTIATE_TEMPLATE

template<class Interaction>
void Molecule::addInteraction(const ParticleName& particleNameI,
                              const ParticleName& particleNameJ,
                              const ParticleName& particleNameK,
                              const Interaction&  interaction)
{
    addInteraction(particleNameI,
                   ResidueName(name_),
                   particleNameJ,
                   ResidueName(name_),
                   particleNameK,
                   ResidueName(name_),
                   interaction);
}

#define ADD_INTERACTION_INSTANTIATE_TEMPLATE(x)                               \
    template void Molecule::addInteraction(const ParticleName& particleNameI, \
                                           const ParticleName& particleNameJ, \
                                           const ParticleName& particleNameK, \
                                           const x&            interaction);
MAP(ADD_INTERACTION_INSTANTIATE_TEMPLATE, SUPPORTED_THREE_CENTER_TYPES)
#undef ADD_INTERACTION_INSTANTIATE_TEMPLATE
//! \endcond

int Molecule::numParticlesInMolecule() const
{
    return particles_.size();
}

void Molecule::addExclusion(const int particleIndex, const int particleIndexToExclude)
{
    // We do not need to add exclusion in case the particle indexes are the same
    // because self exclusion are added by addParticle
    if (particleIndex != particleIndexToExclude)
    {
        exclusions_.emplace_back(particleIndex, particleIndexToExclude);
        exclusions_.emplace_back(particleIndexToExclude, particleIndex);
    }
}

void Molecule::addExclusion(std::tuple<ParticleName, ResidueName> particle,
                            std::tuple<ParticleName, ResidueName> particleToExclude)
{
    // duplication for the swapped pair happens in getExclusions()
    exclusionsByName_.emplace_back(std::make_tuple(std::get<0>(particle),
                                                   std::get<1>(particle),
                                                   std::get<0>(particleToExclude),
                                                   std::get<1>(particleToExclude)));
}

void Molecule::addExclusion(const ParticleName& particleName, const ParticleName& particleNameToExclude)
{
    addExclusion(std::make_tuple(particleName, ResidueName(name_)),
                 std::make_tuple(particleNameToExclude, ResidueName(name_)));
}

const Molecule::InteractionTuple& Molecule::interactionData() const
{
    return interactionData_;
}

const ParticleType& Molecule::at(const std::string& particleTypeName) const
{
    return particleTypes_.at(particleTypeName);
}

ParticleName Molecule::particleName(int i) const
{
    return ParticleName(particles_[i].particleName_);
}

ResidueName Molecule::residueName(int i) const
{
    return ResidueName(particles_[i].residueName_);
}

std::vector<std::tuple<int, int>> Molecule::getExclusions() const
{
    // tuples of (particleName, residueName, index)
    std::vector<std::tuple<std::string, std::string, int>> indexKey;
    indexKey.reserve(numParticlesInMolecule());

    for (int i = 0; i < numParticlesInMolecule(); ++i)
    {
        indexKey.emplace_back(particles_[i].particleName_, particles_[i].residueName_, i);
    }

    std::sort(std::begin(indexKey), std::end(indexKey));

    std::vector<std::tuple<int, int>> ret = exclusions_;
    ret.reserve(exclusions_.size() + exclusionsByName_.size());

    // normal operator<, except ignore third element
    auto sortKey = [](const auto& tup1, const auto& tup2) {
        if (std::get<0>(tup1) < std::get<0>(tup2))
        {
            return true;
        }
        else
        {
            return std::get<1>(tup1) < std::get<1>(tup2);
        }
    };

    // convert exclusions given by names to indices and append
    for (auto& tup : exclusionsByName_)
    {
        const std::string& particleName1 = std::get<0>(tup);
        const std::string& residueName1  = std::get<1>(tup);
        const std::string& particleName2 = std::get<2>(tup);
        const std::string& residueName2  = std::get<3>(tup);

        // look up first index (binary search)
        auto it1 = std::lower_bound(std::begin(indexKey),
                                    std::end(indexKey),
                                    std::make_tuple(particleName1, residueName2, 0),
                                    sortKey);

        // make sure we have the (particleName,residueName) combo
        if (it1 == std::end(indexKey) or std::get<0>(*it1) != particleName1 or std::get<1>(*it1) != residueName1)
        {
            throw std::runtime_error(
                    (std::string("Particle ") += particleName1 + std::string(" in residue ") +=
                     residueName1 + std::string(" not found in list of particles\n")));
        }

        int firstIndex = std::get<2>(*it1);

        // look up second index (binary search)
        auto it2 = std::lower_bound(std::begin(indexKey),
                                    std::end(indexKey),
                                    std::make_tuple(particleName2, residueName2, 0),
                                    sortKey);

        // make sure we have the (particleName,residueName) combo
        if (it2 == std::end(indexKey) or std::get<0>(*it2) != particleName2 or std::get<1>(*it2) != residueName2)
        {
            throw std::runtime_error(
                    (std::string("Particle ") += particleName2 + std::string(" in residue ") +=
                     residueName2 + std::string(" not found in list of particles\n")));
        }

        int secondIndex = std::get<2>(*it2);

        ret.emplace_back(firstIndex, secondIndex);
        ret.emplace_back(secondIndex, firstIndex);
    }

    std::sort(std::begin(ret), std::end(ret));

    auto uniqueEnd = std::unique(std::begin(ret), std::end(ret));
    if (uniqueEnd != std::end(ret))
    {
        printf("[nblib] Warning: exclusionList for molecule %s contained duplicates",
               name_.value().c_str());
    }

    ret.erase(uniqueEnd, std::end(ret));
    return ret;
}

std::unordered_map<std::string, ParticleType> Molecule::particleTypesMap() const
{
    return particleTypes_;
}

std::vector<ParticleData> Molecule::particleData() const
{
    return particles_;
}

} // namespace nblib
