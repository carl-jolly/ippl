//
// Class HaloCells
//   The guard / ghost cells of BareField.
//
// Copyright (c) 2020, Matthias Frey, Paul Scherrer Institut, Villigen PSI, Switzerland
// All rights reserved
//
// This file is part of IPPL.
//
// IPPL is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// You should have received a copy of the GNU General Public License
// along with IPPL. If not, see <https://www.gnu.org/licenses/>.
//
#ifndef IPPL_GUARD_CELLS_H
#define IPPL_GUARD_CELLS_H

#include "Index/NDIndex.h"
#include "Types/ViewTypes.h"
// #include "Communicate/Archive.h"
#include "FieldLayout/FieldLayout.h"

#include <array>

namespace ippl {
    namespace detail {

        template <typename T, unsigned Dim>
        class HaloCells
        {

        public:
            using bound_type = std::array<long, 2 * Dim>;
            using bounds_type = std::array<bound_type, 2 * Dim>;
            using view_type = typename detail::ViewType<T, Dim>::view_type;
            using Layout_t  = FieldLayout<Dim>;

            HaloCells();

            void resize(const view_type&, int nghost);

            /*! lower halo cells (ordering x, y, z)
             * x --> lower y-z plane
             * x --> upper y-z plane
             * y --> lower x-z plane
             * y --> upper x-z plane
             * z --> lower x-y plane
             * z --> upper x-y plane
             */
            auto lowerHalo(view_type&, unsigned int dim);

            auto upperHalo(view_type&, unsigned int dim);

            auto lowerInternal(view_type&, unsigned int dim);

            auto upperInternal(view_type&, unsigned int dim);

            void fillHalo(view_type& view, const T& value);

            void exchangeHalo(view_type&, const Layout_t* layout, int nghost);


            void pack(view_type&, int index) const;
//
//             void unpack(view_type&);

        private:
            /*!
             * @param shift number of shifts to internal cells
             */
            void fillBounds(bounds_type& bounds,
                            const view_type&, int nghost,
                            int shift = 0);

            void fillCorners(const view_type&, int nghost);

            auto lower(bounds_type& bounds, const view_type&, unsigned int dim);
            auto upper(bounds_type& bounds, const view_type&, unsigned int dim);


            /*! lower halo cells (ordering x, y, z)
             * x --> lower y-z plane
             * x --> upper y-z plane
             * y --> lower x-z plane
             * y --> upper x-z plane
             * z --> lower x-y plane
             * z --> upper x-y plane
             */
            bounds_type haloBounds_m;

            /*!
             * These subviews correspond to the internal data that is used
             * to exchange.
             */
            bounds_type internalBounds_m;
        };
    }
}

#include "Field/HaloCells.hpp"

#endif