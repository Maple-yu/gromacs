/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2016, by the GROMACS development team, led by
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
/*! \internal \brief
 * Implements part of the alexandria program.
 * \author  Mohammad Mehdi Ghahremanpour <mohammad.ghahremanpour@icm.uu.se>
 * \author David van der Spoel <david.vanderspoel@icm.uu.se>
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "gromacs/commandline/filenm.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/linearalgebra/matrix.h"
#include "gromacs/listed-forces/bonded.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/math/vectypes.h"
#include "gromacs/statistics/statistics.h"
#include "gromacs/topology/atomprop.h"
#include "gromacs/topology/atoms.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/gmxomp.h"
#include "gromacs/utility/textreader.h"

#include "coulombintegrals/coulombintegrals.h"
#include "nmsimplex.h"
#include "poldata.h"
#include "qgen_resp.h"
#include "regression.h"


namespace alexandria
{

QgenResp::QgenResp()
{
    bAXpRESP_           = false;
    qfac_               = 1e-3;
    bHyper_             = 0.1;
    wtot_               = 0;
    pfac_               = 1;
    bEntropy_           = false;
    rDecrZeta_          = true;
    bFitZeta_           = false;
    zmin_               = 5;
    zmax_               = 100;
    deltaZ_             = 1;
    bRandZeta_          = false;
    rDecrZeta_          = false;
    uniqueQ_            = 0;
    fitQ_               = 0;
    qmin_               = -3;
    qmax_               = 3; /* e */
    bRandQ_             = false;
}
  
void QgenResp::updateAtomCoords(const PaddedRVecVector x)
{
    for (size_t i = 0; i < ra_.size(); i++)
    {
        ra_[i].setX(x[i]);
    }
}

void QgenResp::updateAtomCharges(t_atoms  *atoms)
{
    GMX_RELEASE_ASSERT(static_cast<int>(ra_.size()) == atoms->nr, "Inconsistency between number of resp atoms and topology atoms");
    
    for (size_t i = 0; i < ra_.size(); i++)
    {
        if (!ra_[i].fixedQ())
        {
            ra_[i].setQ(atoms->atom[i].q);
        }
    }
}

void QgenResp::setAtomInfo(t_atoms                   *atoms,
                           const alexandria::Poldata &pd,
                           const PaddedRVecVector     x,
                           const int                  qtotal)
{
    nAtom_  = 0;
    nShell_ = 0;
    qtot_   = qtotal;
    qshell_ = 0;
    ratype_.clear();
    ra_.clear();
    
    // First add all the resp atom types
    for (int i = 0; i < atoms->nr; i++)
    {
        // THIS is a hack
        bool hasShell = ((i < atoms->nr-1) &&
                         (strncmp(*atoms->atomtype[i], *atoms->atomtype[i+1], strlen(*atoms->atomtype[i])) == 0) &&
                         (atoms->atom[i].ptype == eptAtom) &&
                         (atoms->atom[i+1].ptype == eptShell));
        switch (atoms->atom[i].ptype)
        {
            case eptAtom:
                nAtom_ += 1;
                break;
            case eptShell:
                nShell_ += 1;
                break;
            default:
                fprintf(stderr, "Oh dear, particle %d is a %s\n",
                        i, ptype_str[atoms->atom[i].ptype]);
        }
        if (findRAT(atoms->atom[i].type) == endRAT())
        {
            ratype_.push_back(RespAtomType(atoms->atom[i].type,
                                           atoms->atom[i].ptype,
                                           hasShell,
                                           *(atoms->atomtype[i]), pd,
                                           iDistributionModel_, dzatoms_));
        }
    }
    // Then add all the atoms
    for (int i = 0; (i < atoms->nr); i++)
    {
        // Now compute starting charge for atom, taking into account
        // the charges of the other "shells".
        auto   rat = findRAT(atoms->atom[i].type);
        GMX_RELEASE_ASSERT(rat != endRAT(), "Inconsistency setting atom info");
        double q    = rat->beginRZ()->q();
        // q is the variable charge. For precision we determine what the
        // "rest" of the charge on a particle is such that we fit charges to
        // the residual electrostatic potential only.
        double qref = 0;
        if (rat->hasShell())
        {
            // The reference charge is the charge of the shell times -1
            auto ratp = findRAT(atoms->atom[i+1].type);
            GMX_RELEASE_ASSERT(ratp != endRAT(), "Inconsistency setting atom info");
            qref    -= ratp->beginRZ()->q();
            qshell_ += ratp->beginRZ()->q();
        }
        else
        {
            for (auto ra = rat->beginRZ()+1; ra < rat->endRZ(); ++ra)
            {
                qref -= ra->q();
            }
        }
        ra_.push_back(RespAtom(atoms->atom[i].atomnumber,
                               atoms->atom[i].type,
                               q,
                               qref,
                               x[i]));
        if (debug)
        {
            fprintf(debug, "Atom %d hasShell %s q = %g qref = %g\n",
                    i, gmx::boolToString(rat->hasShell()), q, qref);
        }
    }
}

void QgenResp::summary(FILE *fp)
{
    if (nullptr != fp)
    {
        fprintf(fp, "There are %d atoms, %d atomtypes for (R)ESP fitting.\n",
                static_cast<int>(nAtom()),
                static_cast<int>(nAtomType()));
        for (size_t i = 0; (i < nAtom()); i++)
        {
            fprintf(fp, " %d", symmetricAtoms_[i]);
        }
        fprintf(fp, "\n");
    }
}

void QgenResp::setAtomSymmetry(const std::vector<int> &symmetricAtoms)
{
    GMX_RELEASE_ASSERT(!ra_.empty(), "RespAtom vector not initialized");
    GMX_RELEASE_ASSERT(!ratype_.empty(), "RespAtomType vector not initialized");
    GMX_RELEASE_ASSERT(symmetricAtoms.size() == 0 ||
                       symmetricAtoms.size() == static_cast<size_t>(nAtom()),
                       "Please pass me a correct symmetric atoms vector");

    if (symmetricAtoms.size() == 0)
    {
        for (int i = 0; i < nAtom_; i++)
        {
            symmetricAtoms_.push_back(i);
        }
    }
    else
    {
        symmetricAtoms_ = symmetricAtoms;
    }
    uniqueQ_        = 0;
    fitQ_           = 0;
    for (size_t i = 0; (i < nAtom()); i++)
    {
        if (!ra_[i].fixedQ())
        {
            fitQ_ += 1;

            if (symmetricAtoms_[i] == static_cast<int>(i))
            {
                uniqueQ_ += 1;
            }
        }
    }
    if (debug)
    {
        size_t maxz = 0;
        for (size_t i = 0; (i < nAtomType()); i++)
        {
            if (ratype_[i].getNZeta() > maxz)
            {
                maxz = ratype_[i].getNZeta();
            }
        }

        fprintf(debug, "GRQ: %3s %5s", "nr", "type");
        fprintf(debug, " %8s %8s %8s %8s\n", "q", "zeta", "q", "zeta");
        for (size_t i = 0; (i < nAtom()); i++)
        {
            int                  atype = ra_[i].atype();
            RespAtomTypeIterator rai   = findRAT(atype);
            fprintf(debug, "GRQ: %3d %5s", static_cast<int>(i+1),
                    rai->getAtomtype().c_str());
            for (auto zz = rai->beginRZ(); zz <  rai->endRZ(); ++zz)
            {
                fprintf(debug, " %8.4f %8.4f", zz->q(), zz->zeta());
            }
            fprintf(debug, "\n");
        }
    }
    GMX_RELEASE_ASSERT(fitQ_ > 0, "No charges to fit");
}

void QgenResp::writeHisto(const std::string      &fn,
                          const std::string      &title,
                          const gmx_output_env_t *oenv)
{
    FILE       *fp;
    gmx_stats_t gs;
    real       *x, *y;
    int         nbin = 100;

    if (0 == fn.size())
    {
        return;
    }
    gs = gmx_stats_init();
    for (size_t i = 0; (i < nEsp()); i++)
    {
        gmx_stats_add_point(gs, i, gmx2convert(ep_[i].vCalc(), eg2cHartree_e), 0, 0);
    }

    gmx_stats_make_histogram(gs, 0, &nbin, ehistoY, 1, &x, &y);

    fp = xvgropen(fn.c_str(), title.c_str(), "Pot (1/a.u.)", "()", oenv);
    for (int i = 0; (i < nbin); i++)
    {
        fprintf(fp, "%10g  %10g\n", x[i], y[i]);
    }
    free(x);
    free(y);
    fclose(fp);
    gmx_stats_free(gs);
}

void QgenResp::writeDiffCube(QgenResp               &src,
                             const std::string      &cubeFn,
                             const std::string      &histFn,
                             const std::string      &title,
                             const gmx_output_env_t *oenv,
                             int                     rho)
{
    FILE       *fp;
    int         i, m, ix, iy, iz;
    real        pp, q, r, rmin;
    gmx_stats_t gst = nullptr, ppcorr = nullptr;

    if (0 != histFn.size())
    {
        gst    = gmx_stats_init();
        ppcorr = gmx_stats_init();
    }
    if (0 != cubeFn.size())
    {
        fp = gmx_ffopen(cubeFn.c_str(), "w");
        fprintf(fp, "%s\n", title.c_str());
        fprintf(fp, "POTENTIAL\n");
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n",
                static_cast<int>(nAtom()),
                gmx2convert(origin_[XX], eg2cBohr),
                gmx2convert(origin_[YY], eg2cBohr),
                gmx2convert(origin_[ZZ], eg2cBohr));
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", nxyz_[XX],
                gmx2convert(space_[XX], eg2cBohr), 0.0, 0.0);
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", nxyz_[YY],
                0.0, gmx2convert(space_[YY], eg2cBohr), 0.0);
        fprintf(fp, "%5d%12.6f%12.6f%12.6f\n", nxyz_[ZZ],
                0.0, 0.0, gmx2convert(space_[ZZ], eg2cBohr));

        for (size_t m = 0; (m < nAtom()); m++)
        {
            q = ra_[m].q();
            fprintf(fp, "%5d%12.6f%12.6f%12.6f%12.6f\n",
                    ra_[m].atomnumber(), q,
                    gmx2convert(ra_[m].x()[XX], eg2cBohr),
                    gmx2convert(ra_[m].x()[YY], eg2cBohr),
                    gmx2convert(ra_[m].x()[ZZ], eg2cBohr));
        }

        for (ix = m = 0; ix < nxyz_[XX]; ix++)
        {
            for (iy = 0; iy < nxyz_[YY]; iy++)
            {
                for (iz = 0; iz < nxyz_[ZZ]; iz++, m++)
                {
                    if (src.nEsp() > 0)
                    {
                        pp = ep_[m].vCalc() - src.ep_[m].v();
                        if (nullptr != ppcorr)
                        {
                            gmx_stats_add_point(ppcorr,
                                                gmx2convert(src.ep_[m].v(), eg2cHartree_e),
                                                gmx2convert(ep_[m].vCalc(), eg2cHartree_e), 0, 0);
                        }
                    }
                    else
                    {
                        if (rho == 0)
                        {
                            pp = gmx2convert(ep_[m].vCalc(), eg2cHartree_e);
                        }
                        else
                        {
                            pp = ep_[m].rho()*pow(BOHR2NM, 3);
                        }
                    }
                    fprintf(fp, "%13.5e", pp);
                    if (iz % 6 == 5)
                    {
                        fprintf(fp, "\n");
                    }
                    if (nullptr != gst)
                    {
                        rmin = 1000;
                        /* Add point to histogram! */
                        for (auto i = ra_.begin(); i < ra_.end(); ++i)
                        {
                            gmx::RVec dx;
                            rvec_sub(i->x(), ep_[m].esp(), dx);
                            r = norm(dx);
                            if (r < rmin)
                            {
                                rmin = r;
                            }
                        }
                        gmx_stats_add_point(gst, rmin, pp, 0, 0);
                    }
                }
                if ((iz % 6) != 0)
                {
                    fprintf(fp, "\n");
                }
            }
        }
        fclose(fp);
    }
    if (nullptr != gst)
    {
        int   nb = 0;
        real *x  = nullptr, *y = nullptr;

        fp = xvgropen(histFn.c_str(), "Absolute deviation from QM", "Distance (nm)",
                      "Potential", oenv);
        gmx_stats_dump_xy(gst, fp);
        if (0)
        {
            gmx_stats_make_histogram(gst, 0.01, &nb, ehistoX, 0, &x, &y);
            gmx_stats_free(gst);
            for (i = 0; (i < nb); i++)
            {
                fprintf(fp, "%10g  %10g\n", x[i], y[i]);
            }
            free(x);
            free(y);
        }
        fclose(fp);
        fp = xvgropen("diff-pot.xvg", "Correlation between QM and Calc", "Pot (QM)",
                      "Pot (Calc)", oenv);
        gmx_stats_dump_xy(ppcorr, fp);
        fclose(fp);
    }
}

void QgenResp::writeCube(const std::string &fn, const std::string &title)
{
    QgenResp dummy;
    writeDiffCube(dummy,  fn, nullptr, title, nullptr, 0);
}

void QgenResp::writeRho(const std::string &fn, const std::string &title)
{
    QgenResp dummy;
    writeDiffCube(dummy,  fn, nullptr, title, nullptr, 1);
}

void QgenResp::readCube(const std::string &fn, bool bESPonly)
{
    int                 natom, nxyz[DIM] = { 0, 0, 0 };
    double              space[DIM] = { 0, 0, 0 };
    std::vector<double> pot;

    gmx::TextReader     tr(fn);
    std::string         tmp;
    int                 line = 0;
    bool                bOK  = true;
    while (bOK && tr.readLine(&tmp))
    {
        while (!tmp.empty() && tmp[tmp.length()-1] == '\n')
        {
            tmp.erase(tmp.length()-1);
        }
        if (0 == line)
        {
            printf("%s\n", tmp.c_str());
        }
        else if (1 == line && tmp.compare("POTENTIAL") != 0)
        {
            bOK = false;
        }
        else if (2 == line)
        {
            double origin[DIM];
            bOK = (4 == sscanf(tmp.c_str(), "%d%lf%lf%lf",
                               &natom, &origin[XX], &origin[YY], &origin[ZZ]));
            if (bOK && !bESPonly)
            {
                origin_[XX] = origin[XX];
                origin_[YY] = origin[YY];
                origin_[ZZ] = origin[ZZ];
            }
        }
        else if (3 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%lf",
                               &nxyz[XX], &space[XX]));
        }
        else if (4 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%*s%lf",
                               &nxyz[YY], &space[YY]));
        }
        else if (5 == line)
        {
            bOK = (2 == sscanf(tmp.c_str(), "%d%*s%*s%lf",
                               &nxyz[ZZ], &space[ZZ]));
            if (bOK)
            {
                for (int m = 0; (m < DIM); m++)
                {
                    nxyz_[m]  = nxyz[m];
                    space_[m] = space[m];
                }
                for (int m = 0; (m < DIM); m++)
                {
                    origin_[m] = convert2gmx(origin_[m], eg2cBohr);
                    space_[m]  = convert2gmx(space_[m], eg2cBohr);
                }
            }
            pot.clear();
        }
        else if (line >= 6 && line < 6+natom)
        {
            double lx, ly, lz, qq;
            int    anr, m = line - 6;
            bOK = (5 == sscanf(tmp.c_str(), "%d%lf%lf%lf%lf",
                               &anr, &qq, &lx, &ly, &lz));
            if (bOK)
            {
                if (!bESPonly)
                {
                    ra_[m].setAtomnumber(anr);
                    ra_[m].setQ(qq);
                }
                gmx::RVec xx;
                xx[XX] = convert2gmx(lx, eg2cBohr);
                xx[YY] = convert2gmx(ly, eg2cBohr);
                xx[ZZ] = convert2gmx(lz, eg2cBohr);
                ra_[m].setX(xx);
            }
        }
        else if (line >= 6+natom)
        {
            std::vector<std::string> ss = gmx::splitString(tmp);
            for (const auto &s : ss)
            {
                pot.push_back(convert2gmx(atof(s.c_str()), eg2cHartree_e));
            }
        }

        line++;
    }
    if (bOK)
    {
        ep_.clear();
        int m = 0;
        for (int ix = 0; ix < nxyz_[XX]; ix++)
        {
            for (int iy = 0; iy < nxyz_[YY]; iy++)
            {
                for (int iz = 0; iz < nxyz_[ZZ]; iz++, m++)
                {
                    gmx::RVec e;
                    e[XX] = origin_[XX] + ix*space_[XX];
                    e[YY] = origin_[YY] + iy*space_[YY];
                    e[ZZ] = origin_[ZZ] + iz*space_[ZZ];

                    ep_.push_back(EspPoint(e, pot[m]));
                }
            }
        }
    }
    if (!bOK)
    {
        gmx_fatal(FARGS, "Error reading %s. Found %d potential values, %d coordinates and %d atoms",
                  fn.c_str(), static_cast<int>(pot.size()), static_cast<int>(ep_.size()),
                  static_cast<int>(ra_.size()));
    }
}

void QgenResp::copyGrid(QgenResp &src)
{
    int m;

    for (m = 0; (m < DIM); m++)
    {
        origin_[m] = src.origin_[m];
        space_[m]  = src.space_[m];
        nxyz_[m]   = src.nxyz_[m];
    }
    int nesp = src.nEsp();
    ep_.clear();
    for (m = 0; (m < nesp); m++)
    {
        ep_.push_back(src.ep_[m]);
    }
}

void QgenResp::makeGrid(real spacing, matrix box, rvec x[])
{
    if (0 != nEsp())
    {
        fprintf(stderr, "Overwriting existing ESP grid\n");
    }
    if (spacing <= 0)
    {
        spacing = 0.1;
        fprintf(stderr, "spacing too small, setting it to %g\n", spacing);
    }
    for (size_t i = 0; (i < nAtom()); i++)
    {
        ra_[i].setX(x[i]);
    }
    for (int m = 0; (m < DIM); m++)
    {
        nxyz_[m]  = 1+(int) (box[m][m]/spacing);
        space_[m] = box[m][m]/nxyz_[m];
    }
    ep_.clear();
    for (int i = 0; (i < nxyz_[XX]); i++)
    {
        gmx::RVec xyz;
        xyz[XX] = (i-0.5*nxyz_[XX])*space_[XX];
        for (int j = 0; (j < nxyz_[YY]); j++)
        {
            xyz[YY] = (j-0.5*nxyz_[YY])*space_[YY];
            for (int k = 0; (k < nxyz_[ZZ]); k++)
            {
                xyz[ZZ] = (k-0.5*nxyz_[ZZ])*space_[ZZ];
                ep_.push_back(EspPoint(xyz, 0));
            }
        }
    }
}

void QgenResp::calcRho()
{
    double pi32 = pow(M_PI, -1.5);
    for (size_t i = 0; (i < nEsp()); i++)
    {
        double V = 0;
        for (const auto &ra : ra_)
        {
            double               vv = 0;
            gmx::RVec            dx;
            rvec_sub(ep_[i].esp(), ra.x(), dx);
            double               r     = norm(dx);
            int                  atype = ra.atype();
            RespAtomTypeIterator rat   = findRAT(atype);
            GMX_RELEASE_ASSERT(rat == endRAT(), "Can not find atomtype");
            switch (iDistributionModel_)
            {
                case eqdYang:
                case eqdRappe:
                    vv = ra.q()*Nuclear_SS(r,
                                           rat->beginRZ()->row(),
                                           rat->beginRZ()->zeta());
                    break;
                case eqdAXpg:
                case eqdAXg:
                    vv = 0;
                    for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                    {
                        real z = k->zeta();
                        real q = k->q();
                        // TODO Check
                        if (q == 0)
                        {
                            q = ra.q();
                        }
                        if (z > 0 && q != 0)
                        {
                            vv -= (q*pi32*exp(-gmx::square(r*z))*
                                   pow(z, 3));
                        }
                    }
                    break;
                case eqdBultinck:
                case eqdAXp:
                case eqdAXs:
                default:
                    gmx_fatal(FARGS, "Krijg nou wat, iDistributionModel = %d!", iDistributionModel_);
            }
            V  += vv;
        }
        ep_[i].setRho(V);
    }
}

void QgenResp::addEspPoint(double x, double y,
                           double z, double V)
{
    gmx::RVec rv(x, y, z);
    ep_.push_back(EspPoint(rv, V));
}

real QgenResp::myWeight(int iatom) const
{
    if (iatom < nAtom_)
    {
        return watoms_;
    }
    else
    {
        return 1.0;
    }
}

void QgenResp::plotLsq(const gmx_output_env_t *oenv)
{
    real        x, y;
    const char *leg = "Alexandria";
    gmx_stats_t lsq = gmx_stats_init();;    
    for (size_t i = 0; i < nEsp(); i++)
    {
        gmx_stats_add_point(lsq,
                            gmx2convert(ep_[i].v(), eg2cHartree_e),
                            gmx2convert(ep_[i].vCalc(), eg2cHartree_e),
                            0, 0);
    }   
    FILE *fp = xvgropen("EspFit.xvg", "Electrostatic Potential (Hartree/e)", "QM", "Calc", oenv);
    xvgr_legend(fp, 1, &leg, oenv);
    xvgr_line_props(fp, 0, elNone, ecBlack, oenv);
    fprintf(fp, "@ s%d symbol %d\n", 0, 1);   
    fprintf(fp, "@type xy\n");
    while (gmx_stats_get_point(lsq, &x, &y, nullptr, nullptr, 0) == estatsOK)
    {
        fprintf(fp, "%10g  %10g\n", x, y);
    }
    fprintf(fp, "&\n");
    gmx_stats_free(lsq);
}

double QgenResp::calcPenalty()
{
    double p, b2;

    p = 0;
    /* Check for excessive charges */
    for (auto &ra : ra_)
    {
        real                 qi    = ra.q();
        int                  atype = ra.atype();
        RespAtomTypeIterator rat   = findRAT(atype);
        for (auto z = rat->beginRZ(); z < rat->endRZ(); ++z)
        {
            qi += z->q();
        }
        if (qi < qmin_)
        {
            p += gmx::square(qmin_ - qi);
        }
        else if (qi > qmax_)
        {
            p += gmx::square(qmax_ - qi);
        }
        else if ((qi < -0.02) && (ra.atomnumber() == 1))
        {
            p += qi*qi;
        }
    }
    p *= pfac_;
    if (bAXpRESP_ && (iDistributionModel_ == eqdAXp))
    {
        b2 = gmx::square(bHyper_);
        for (size_t i = 0; (i < nAtom()); i++)
        {
            p += sqrt(gmx::square(ra_[i].q()) + b2) - bHyper_;
        }
        p = (qfac_ * p);
    }
    penalty_ = p;

    return penalty_;
}

void QgenResp::regularizeCharges()
{
    double qtot   = 0;
    int    nfixed = 0;
    for (size_t ii = 0; ii < nAtom(); ii++)
    {
        auto rat = findRAT(ra_[ii].atype());
        GMX_RELEASE_ASSERT(rat != endRAT(), "Inconsistency with atomtypes");

        qtot += ra_[ii].q();
        if (ra_[ii].fixedQ())
        {
            nfixed++;
        }
        if (!rat->hasShell())
        {
            qtot -= ra_[ii].qRef();
        }
    }
    double dq = (qtot_ - qtot)/(nAtom()-nfixed);
    for (size_t ii = 0; ii < nAtom(); ii++)
    {
        if (!ra_[ii].fixedQ())
        {
            ra_[ii].setQ(ra_[ii].q() + dq);
        }
    }
}

void QgenResp::calcRms()
{
    double pot2, s2, sum2, entropy;

    pot2 = sum2 = entropy = 0;
    for (size_t i = 0; (i < nEsp()); i++)
    {
        double diff = ep_[i].v() - ep_[i].vCalc();
        if (debug && (i < 4*nAtom()))
        {
            fprintf(debug, "ESP %zu QM: %g FIT: %g DIFF: %g\n",
                    i, ep_[i].v(), ep_[i].vCalc(), diff);
        }
        s2    = gmx::square(diff);
        if ((s2 > 0) && (bEntropy_))
        {
            entropy += s2*log(s2);
        }
        sum2 += s2;
        pot2 += gmx::square(ep_[i].v());
    }
    wtot_ = nEsp();
    if (wtot_ > 0)
    {
        rms_     = gmx2convert(sqrt(sum2/wtot_), eg2cHartree_e);
        entropy_ = gmx2convert(entropy/wtot_, eg2cHartree_e);
    }
    else
    {
        rms_     = 0;
        entropy_ = 0;
    }
    rrms_ = sqrt(sum2/pot2);
}

real QgenResp::getRms(real *wtot, real *rrms)
{
    calcRms();
    *wtot = wtot_;
    *rrms = rrms_;
    if (bEntropy_)
    {
        return entropy_;
    }
    else
    {
        return rms_;
    }
}


double QgenResp::calcJ(ChargeDistributionModel iChargeDistributionModel,
                       rvec                    espx, 
                       rvec                    rax,
                       double                  zeta,
                       int                     row)
{
    rvec   dx;
    double r    = 0;
    double eTot = 0;

    rvec_sub(espx, rax, dx);
    r = norm(dx);
    if (zeta <= 0)
    {
        iChargeDistributionModel = eqdAXp;
    }
    if(watoms_ == 0 && r == 0)
    {
        gmx_fatal(FARGS, "Zero distance between the atom and the grid.");
    }
    switch (iChargeDistributionModel)
    {
        case eqdAXp:
        case eqdAXpp:
            eTot = (1.0/r);
            break;
        case eqdAXs:
        case eqdAXps:
        case eqdRappe:
        case eqdYang:        
            eTot = Nuclear_SS(r, row, zeta);
            break;
        case eqdAXg:
        case eqdAXpg:
            eTot = Nuclear_GG(r, zeta);
            break;
        default:
            gmx_fatal(FARGS, "Unsupported charge model %d", iChargeDistributionModel);
    }
    return (ONE_4PI_EPS0*eTot);
}

void QgenResp::calcPot()
{  
    for (auto &ep : ep_)
    {
        ep.setVCalc(0);
    }     
    auto nthreads = gmx_omp_get_max_threads();    
#pragma omp parallel
    {
        auto thread_id = gmx_omp_get_thread_num();
        auto i0        = thread_id*nEsp()/nthreads;
        auto i1        = std::min(nEsp(), (thread_id+1)*nEsp()/nthreads);
        for (auto i = i0; i < i1; i++)
        {
            double vv  = 0;
            auto espx  = ep_[i].esp();
            auto espv  = ep_[i].v();
            for (auto &ra : ra_)
            {
                auto  atype = ra.atype();
                auto  rat   = findRAT(atype);
                auto  rax   = ra.x();
                for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                {
                    auto q = k->q();
                    if (q == 0)
                    {
                        q = ra.q();
                    }
                    auto epot = calcJ(iDistributionModel_, espx, rax, k->zeta(), k->row());
                    vv += (q*epot);
                }
            }
            ep_[i].setVCalc(vv);
        }
    }
}

void QgenResp::optimizeCharges()
{
    // Increase number of rows for the symmetric atoms. E.g.
    // if we know that atoms 2, 3 and 4 have the same charge we
    // add two equation q2 - q3 = 0 and q2 - q4 = 0.
    // An extra row is needed to fix the total charge.
    int                   nrow     = nEsp() + 1 + fitQ_ - uniqueQ_;
    int                   factor   = nEsp();
    int                   ncolumn  = fitQ_;
    double              **lhs      = alloc_matrix(ncolumn, nrow);
    std::vector<double>   rhs;

    if (nEsp() < nAtom())
    {
        printf("WARNING: Only %zu ESP points for %zu atoms. Cannot generate charges.\n", nEsp(), nAtom());
        return;
    }
    for (size_t j = 0; j < nEsp(); j++)
    {
        rhs.push_back(ep_[j].v());
    }    
    int i = 0;
    for (size_t ii = 0; ii < nAtom(); ii++)
    {
        auto atype = ra_[ii].atype();
        auto rat   = findRAT(atype);
        if (rat->ptype() == eptAtom)
        {
            auto rax = ra_[ii].x();
            for (size_t j = 0; j < nEsp(); j++)
            {
                auto espx  = ep_[j].esp();
                for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                {
                    auto pot = calcJ(iDistributionModel_, espx, rax, k->zeta(), k->row());
                    lhs[i][j] += pot;

                    if (debug && j < 4*nAtom())
                    {
                        fprintf(debug, "Core[%zu] ESP[%zu] espx = %g espy = %g espz = %g V= %g  CalcV=%g\n", 
                                ii, j, espx[XX], espx[YY], espx[ZZ], ep_[j].v(), pot);
                    }
                }
            }
            lhs[i][factor] = factor;
            i++;
        }
        else if (rat->ptype() == eptShell)
        {
            auto rax = ra_[ii].x();
            for (size_t j = 0; j < nEsp(); j++)
            {
                auto espx  = ep_[j].esp();
                for (auto k = rat->beginRZ(); k < rat->endRZ(); ++k)
                {
                    auto pot = calcJ(iDistributionModel_, espx, rax, k->zeta(), k->row());
                    auto q   = k->q();
                    rhs[j]  -= (q*pot);
                    
                    if (debug && j < 4*nAtom())
                    {
                        fprintf(debug, "Shell[%zu] ESP[%zu] Shellx = %g shelly = %g shellz = %g pot= %g q=%g qpot=%g\n", 
                                ii, j, rax[XX], rax[YY], rax[ZZ], pot, q, q*pot);
                    }
                }
            }
        }
    }
    
    // Add the total charge    
    rhs.push_back(factor * (qtot_ - qshell_));
    
    // Add the equations to ascertain symmetric charges
    // We store the index of the cores in ii1. 
    std::vector<int> ii1;
    int    j1     = factor+1;
    int    i1     = 0;  
    for (int i = 0; i < static_cast<int>(nAtom()); i++)
    {
        ii1.push_back(i1);
        if (!ra_[i].fixedQ())
        {
            i1++;
        }
    }    
    for (int i = 0; i < static_cast<int>(nAtom()); i++)
    {
        if (symmetricAtoms_[i] < i)
        {
            lhs[ii1[i]][j1]                   =  factor;
            lhs[ii1[symmetricAtoms_[i]]][j1]  = -factor;
            rhs.push_back(0);
            j1++;            
        }
        
    }    
    GMX_RELEASE_ASSERT(j1 == static_cast<int>(rhs.size()), "Inconsistency adding equations for symmetric charges");
    GMX_RELEASE_ASSERT(j1 == nrow, "Something fishy adding equations for symmetric charges");
    
    if (debug)
    {
        fprintf(debug, "ncolumn = %d nrow = %d point = %zu nfixed = %d nUnique = %d\n", 
                ncolumn, nrow, nEsp(), fitQ_, uniqueQ_);
        for (int i = 0; i < nrow; i++)
        {
            fprintf(debug, "ROW: %d", i);
            for (int j = 0; j < ncolumn; j++)
            {
                fprintf(debug, "  %8g", lhs[j][i]);
            }
            fprintf(debug, "  %8g\n", rhs[i]);
        }
        fprintf(debug, "QCore in the r.h.s:%2g\n", rhs[nrow-1]);
        fprintf(debug, "Qtot:%2d\n",   qtot_);
        fprintf(debug, "QShell:%2d\n", qshell_);
    }

    // Fit the charge
    std::vector<double> q;
    q.resize(ncolumn);
    multi_regression2(nrow, rhs.data(), ncolumn, lhs, q.data());
    if (debug)
    {
        fprintf(debug, "Fitted Charges from optimizeCharges\n");
    }
    i = 0;
    for (size_t ii = 0; ii < nAtom(); ii++)
    {
        if (!ra_[ii].fixedQ())
        {
            ra_[ii].setQ(q[i]);
            if (debug)
            {
                fprintf(debug, "q[%d] = %0.3f\n", i, ra_[ii].q());
            }
            i++;
        }
    }
    free_matrix(lhs);
    regularizeCharges();
}

void QgenResp::potcomp(const std::string      &potcomp,
                       const std::string      &pdbdiff,
                       const gmx_output_env_t *oenv)
{
    double  pp, exp, eem;
    FILE   *fp;
    int     unit = eg2cHartree_e;

    if (0 != potcomp.size())
    {
        const char *pcleg[2] = { "Atoms", "ESP points" };
        fp = xvgropen(potcomp.c_str(), "Electrostatic potential", unit2string(unit), unit2string(unit), oenv);
        xvgr_legend(fp, 2, pcleg, oenv);
        fprintf(fp, "@type xy\n");
        for (size_t i = 0; (i < nEsp()); i++)
        {
            /* Conversion may or may not be in vain depending on unit */
            exp = gmx2convert(ep_[i].v(), unit);
            eem = gmx2convert(ep_[i].vCalc(), unit);
            if (i == nAtom())
            {
                fprintf(fp, "&\n");
                fprintf(fp, "@type xy\n");
            }
            fprintf(fp, "%10.5e  %10.5e\n", exp, eem);
        }
        fprintf(fp, "&\n");
        fclose(fp);
    }
    if (0 != pdbdiff.c_str())
    {
        fp = fopen(pdbdiff.c_str(), "w");
        fprintf(fp, "REMARK All distances are scaled by a factor of two.\n");
        for (size_t i = 0; (i < nEsp()); i++)
        {
            exp = gmx2convert(ep_[i].v(), eg2cHartree_e);
            eem = gmx2convert(ep_[i].vCalc(), eg2cHartree_e);
            pp  = ep_[i].v()-ep_[i].vCalc();
            const gmx::RVec esp = ep_[i].esp();
            fprintf(fp, "%-6s%5u  %-4.4s%3.3s %c%4d%c   %8.3f%8.3f%8.3f%6.2f%6.2f\n",
                    "ATOM", 1, "HE", "HE", ' ', static_cast<int>(i+1),
                    ' ', 20*esp[XX], 20*esp[YY], 20*esp[ZZ], 0.0, pp);
        }
        fclose(fp);
    }
}

double QgenResp::getAtomCharge(int atom) const
{
    range_check(atom, 0, nAtom());
    double                    q     = ra_[atom].q();
    // TODO Check
    if (1)
    {
        int                       atype = ra_[atom].atype();
        RespAtomTypeConstIterator rat   = findRAT(atype);
        for (auto z = rat->beginRZ()+1; z < rat->endRZ(); ++z)
        {
            q += z->q();
        }
    }
    return q;
}

double QgenResp::getCharge(int atom, size_t zz) const
{
    range_check(atom, 0, nAtom());
    double                    q     = ra_[atom].q();
    int                       atype = ra_[atom].atype();
    RespAtomTypeConstIterator rat   = findRAT(atype);
    if (zz < rat->getNZeta())
    {
        q = (rat->beginRZ()+zz)->q();
    }
    return q;
}

double QgenResp::getZeta(int atom, int zz) const
{
    range_check(atom, 0, nAtom());
    int atype                     = ra_[atom].atype();
    RespAtomTypeConstIterator rat = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());

    return (rat->beginRZ()+zz)->zeta();
}

void QgenResp::setCharge(int atom, int zz, double q)
{
    range_check(atom, 0, nAtom());
    int                  atype = ra_[atom].atype();
    RespAtomTypeIterator rat   = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());
    (rat->beginRZ()+zz)->setQ(q);
}

void QgenResp::setZeta(int atom, int zz, double zeta)
{
    range_check(atom, 0, nAtom());
    int                  atype = ra_[atom].atype();
    RespAtomTypeIterator rat   = findRAT(atype);
    range_check(zz, 0, rat->getNZeta());
    (rat->beginRZ()+zz)->setZeta(zeta);
}

void QgenResp::updateZeta(t_atoms *atoms, const Poldata &pd)
{
    int     zz = 0;
    double zeta = 0;
    for (size_t i = 0; i < nAtom(); i++)
    {
        zz   = 0;
        zeta = 0;
        if (atoms->atom[i].ptype == eptShell)
        {
            std::string atomtype     = *(atoms->atomtype[i]);
            size_t      shell_name   = atomtype.find("_s");
            std::string atomtype_new = atomtype;
            if (shell_name != std::string::npos)
            {
                zz = 1;
                atomtype_new = atomtype.substr(0, shell_name);
            }
            zeta = pd.getZeta(iDistributionModel_, atomtype_new, zz);
        }
        else if (atoms->atom[i].ptype == eptAtom)
        {
            zeta = pd.getZeta(iDistributionModel_, *(atoms->atomtype[i]), zz);
        }        
        setZeta(static_cast<int>(i), 0, zeta);
    }
}

} // namespace
