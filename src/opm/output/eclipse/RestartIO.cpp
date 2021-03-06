/*
  Copyright (c) 2016 Statoil ASA
  Copyright (c) 2013-2015 Andreas Lauser
  Copyright (c) 2013 SINTEF ICT, Applied Mathematics.
  Copyright (c) 2013 Uni Research AS
  Copyright (c) 2015 IRIS AS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <string>
#include <vector>

#include <opm/parser/eclipse/EclipseState/Grid/EclipseGrid.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/Schedule.hpp>
#include <opm/parser/eclipse/EclipseState/Tables/Eqldims.hpp>

#include <opm/output/eclipse/RestartIO.hpp>

#include <ert/ecl/EclKW.hpp>
#include <ert/ecl/FortIO.hpp>
#include <ert/ecl/EclFilename.hpp>
#include <ert/ecl/ecl_kw_magic.h>
#include <ert/ecl/ecl_init_file.h>
#include <ert/ecl/ecl_file.h>
#include <ert/ecl/ecl_kw.h>
#include <ert/ecl/ecl_type.h>
#include <ert/ecl/ecl_grid.h>
#include <ert/ecl/ecl_util.h>
#include <ert/ecl/ecl_rft_file.h>
#include <ert/ecl/ecl_file_view.h>
#include <ert/ecl_well/well_const.h>
#include <ert/ecl/ecl_rsthead.h>
#include <ert/util/util.h>
#define OPM_XWEL      "OPM_XWEL"
#define OPM_IWEL      "OPM_IWEL"

namespace Opm {
namespace RestartIO  {



namespace {

    static const int NIWELZ = 11; //Number of data elements per well in IWEL array in restart file
    static const int NZWELZ = 3;  //Number of 8-character words per well in ZWEL array restart file
    static const int NICONZ = 15; //Number of data elements per connection in ICON array restart file

    /**
     * The constants NIWELZ and NZWELZ referes to the number of
     * elements per well that we write to the IWEL and ZWEL eclipse
     * restart file data arrays. The constant NICONZ refers to the
     * number of elements per connection in the eclipse restart file
     * ICON data array.These numbers are written to the INTEHEAD
     * header.
     *
     * Observe that all of these values are our "current-best-guess"
     * for how many numbers are needed; there might very well be third
     * party applications out there which have a hard expectation for
     * these values.
     */


    inline int to_ert_welltype( const Well& well, size_t timestep ) {
        if( well.isProducer( timestep ) ) return IWEL_PRODUCER;

        switch( well.getInjectionProperties( timestep ).injectorType ) {
        case WellInjector::WATER:
            return IWEL_WATER_INJECTOR;
        case WellInjector::GAS:
            return IWEL_GAS_INJECTOR;
        case WellInjector::OIL:
            return IWEL_OIL_INJECTOR;
        default:
            return IWEL_UNDOCUMENTED_ZERO;
        }
    }

    std::vector<double> double_vector( const ecl_kw_type * ecl_kw ) {
        size_t size = ecl_kw_get_size( ecl_kw );

        if (ecl_type_get_type( ecl_kw_get_data_type( ecl_kw ) ) == ECL_DOUBLE_TYPE ) {
            const double * ecl_data = ecl_kw_get_double_ptr( ecl_kw );
            return { ecl_data , ecl_data + size };
        } else {
            const float * ecl_data = ecl_kw_get_float_ptr( ecl_kw );
            return { ecl_data , ecl_data + size };
        }

    }


    inline data::Solution restoreSOLUTION( ecl_file_view_type* file_view,
                                           const std::vector<RestartKey>& solution_keys,
                                           int numcells) {

        data::Solution sol( false );
        for (const auto& value : solution_keys) {
            const std::string& key = value.key;
            UnitSystem::measure dim = value.dim;
            bool required = value.required;

            if( !ecl_file_view_has_kw( file_view, key.c_str() ) ) {
                if (required)
                    throw std::runtime_error("Read of restart file: "
                                             "File does not contain "
                                             + key
                                             + " data" );
                else
                    continue;
            }

            const ecl_kw_type * ecl_kw = ecl_file_view_iget_named_kw( file_view , key.c_str() , 0 );
            if( ecl_kw_get_size(ecl_kw) != numcells)
                throw std::runtime_error("Restart file: Could not restore "
                                         + std::string( ecl_kw_get_header( ecl_kw ) )
                                         + ", mismatched number of cells" );

            std::vector<double> data = double_vector( ecl_kw );
            sol.insert( key, dim, data , data::TargetType::RESTART_SOLUTION );
        }

        return sol;
    }


using rt = data::Rates::opt;
data::Wells restore_wells( const ecl_kw_type * opm_xwel,
                           const ecl_kw_type * opm_iwel,
                           int sim_step,
                           const EclipseState& es,
                           const EclipseGrid& grid,
                           const Schedule& schedule) {

    const auto& sched_wells = schedule.getWells( sim_step );
    std::vector< rt > phases;
    {
        const auto& phase = es.runspec().phases();
        if( phase.active( Phase::WATER ) ) phases.push_back( rt::wat );
        if( phase.active( Phase::OIL ) )   phases.push_back( rt::oil );
        if( phase.active( Phase::GAS ) )   phases.push_back( rt::gas );
    }

    const auto well_size = [&]( size_t acc, const Well* w ) {
        return acc
            + 2 + phases.size()
            + (  w->getConnections( sim_step ).size()
              * (phases.size() + data::Connection::restart_size) );
    };

    const auto expected_xwel_size = std::accumulate( sched_wells.begin(),
                                                     sched_wells.end(),
                                                     0,
                                                     well_size );

    if( ecl_kw_get_size( opm_xwel ) != expected_xwel_size ) {
        throw std::runtime_error(
                "Mismatch between OPM_XWEL and deck; "
                "OPM_XWEL size was " + std::to_string( ecl_kw_get_size( opm_xwel ) ) +
                ", expected " + std::to_string( expected_xwel_size ) );
    }

    if( ecl_kw_get_size( opm_iwel ) != int(sched_wells.size()) )
        throw std::runtime_error(
                "Mismatch between OPM_IWEL and deck; "
                "OPM_IWEL size was " + std::to_string( ecl_kw_get_size( opm_iwel ) ) +
                ", expected " + std::to_string( sched_wells.size() ) );

    data::Wells wells;
    const double * opm_xwel_data = ecl_kw_get_double_ptr( opm_xwel );
    const int * opm_iwel_data = ecl_kw_get_int_ptr( opm_iwel );
    for( const auto* sched_well : sched_wells ) {
        data::Well& well = wells[ sched_well->name() ];

        well.bhp = *opm_xwel_data++;
        well.temperature = *opm_xwel_data++;
        well.control = *opm_iwel_data++;

        for( auto phase : phases )
            well.rates.set( phase, *opm_xwel_data++ );

        for( const auto& sc : sched_well->getConnections( sim_step ) ) {
            const auto i = sc.getI(), j = sc.getJ(), k = sc.getK();
            if( !grid.cellActive( i, j, k ) || sc.state == WellCompletion::SHUT ) {
                opm_xwel_data += data::Connection::restart_size + phases.size();
                continue;
            }

            const auto active_index = grid.activeIndex( i, j, k );

            well.connections.emplace_back();
            auto& connection = well.connections.back();
            connection.index = active_index;
            connection.pressure = *opm_xwel_data++;
            connection.reservoir_rate = *opm_xwel_data++;
            for( auto phase : phases )
                connection.rates.set( phase, *opm_xwel_data++ );
        }
    }

    return wells;
}
}

/* should take grid as argument because it may be modified from the simulator */
RestartValue load( const std::string& filename,
                   int report_step,
                   const std::vector<RestartKey>& solution_keys,
                   const EclipseState& es,
                   const EclipseGrid& grid,
                   const Schedule& schedule,
                   const std::vector<RestartKey>& extra_keys) {

    int sim_step = std::max(report_step - 1, 0);
    const bool unified                   = ( ERT::EclFiletype( filename ) == ECL_UNIFIED_RESTART_FILE );
    ERT::ert_unique_ptr< ecl_file_type, ecl_file_close > file(ecl_file_open( filename.c_str(), 0 ));
    ecl_file_view_type * file_view;

    if( !file )
        throw std::runtime_error( "Restart file " + filename + " not found!" );

    if( unified ) {
        file_view = ecl_file_get_restart_view( file.get() , -1 , report_step , -1 , -1 );
        if (!file_view)
            throw std::runtime_error( "Restart file " + filename
                                      + " does not contain data for report step "
                                      + std::to_string( report_step ) + "!" );
    } else
        file_view = ecl_file_get_global_view( file.get() );

    const ecl_kw_type * intehead = ecl_file_view_iget_named_kw( file_view , "INTEHEAD", 0 );
    const ecl_kw_type * opm_xwel = ecl_file_view_iget_named_kw( file_view , "OPM_XWEL", 0 );
    const ecl_kw_type * opm_iwel = ecl_file_view_iget_named_kw( file_view, "OPM_IWEL", 0 );

    UnitSystem units( static_cast<ert_ecl_unit_enum>(ecl_kw_iget_int( intehead , INTEHEAD_UNIT_INDEX )));
    RestartValue rst_value( restoreSOLUTION( file_view, solution_keys, grid.getNumActive( )),
                            restore_wells( opm_xwel, opm_iwel, sim_step , es, grid, schedule));

    for (const auto& extra : extra_keys) {
        const std::string& key = extra.key;
        bool required = extra.required;

        if (ecl_file_view_has_kw( file_view , key.c_str())) {
            const ecl_kw_type * ecl_kw = ecl_file_view_iget_named_kw( file_view , key.c_str() , 0 );
            const double * data_ptr = ecl_kw_get_double_ptr( ecl_kw );
            const double * end_ptr  = data_ptr + ecl_kw_get_size( ecl_kw );
            rst_value.addExtra(key, extra.dim, {data_ptr, end_ptr});
        } else if (required)
            throw std::runtime_error("No such key in file: " + key);

    }

    // Convert solution fields and extra data from user units to SI
    rst_value.solution.convertToSI(units);
    for (auto & extra_value : rst_value.extra) {
        const auto& restart_key = extra_value.first;
        auto & data = extra_value.second;

        units.to_si(restart_key.dim, data);
    }

    return rst_value;
}





namespace {

std::vector<int> serialize_ICON( int sim_step,
                                 int ncwmax,
                                 const std::vector<const Well*>& sched_wells,
                                 const EclipseGrid& /*grid*/) {

    size_t well_offset = 0;
    std::vector<int> data( sched_wells.size() * ncwmax * NICONZ , 0 );
    for (const Well* well : sched_wells) {
        const auto& connections = well->getConnections( sim_step );
        size_t connection_offset = 0;
        for( const auto& connection : connections) {
            size_t offset = well_offset + connection_offset;
            data[ offset + ICON_IC_INDEX ] = 1;

            data[ offset + ICON_I_INDEX ] = connection.getI() + 1;
            data[ offset + ICON_J_INDEX ] = connection.getJ() + 1;
            data[ offset + ICON_K_INDEX ] = connection.getK() + 1;
            data[ offset + ICON_DIRECTION_INDEX ] = connection.dir;
            {
                const auto open = WellCompletion::StateEnum::OPEN;
                data[ offset + ICON_STATUS_INDEX ] = connection.state == open
                    ? 1
                    : 0;
            }

            connection_offset += NICONZ;
        }
        well_offset += ncwmax * NICONZ;
    }
    return data;
}

std::vector<int> serialize_IWEL( size_t step,
                                 const std::vector<const Well *>& wells,
                                 const EclipseGrid& grid) {

    std::vector<int> data( wells.size() * NIWELZ , 0 );
    size_t offset = 0;
    for (const auto well : wells) {
        const auto& connections = well->getActiveConnections( step, grid );

        data[ offset + IWEL_HEADI_INDEX ] = well->getHeadI( step ) + 1;
        data[ offset + IWEL_HEADJ_INDEX ] = well->getHeadJ( step ) + 1;
        data[ offset + IWEL_CONNECTIONS_INDEX ] = connections.size();
        data[ offset + IWEL_GROUP_INDEX ] = 1;

        data[ offset + IWEL_TYPE_INDEX ] = to_ert_welltype( *well, step );
        data[ offset + IWEL_STATUS_INDEX ] =
            well->getStatus( step ) == WellCommon::OPEN ? 1 : 0;

        offset += NIWELZ;
    }
    return data;
}










std::vector< int > serialize_OPM_IWEL( const data::Wells& wells,
                                       const std::vector< const Well* >& sched_wells ) {

    const auto getctrl = [&]( const Well* w ) {
        const auto itr = wells.find( w->name() );
        return itr == wells.end() ? 0 : itr->second.control;
    };

    std::vector< int > iwel( sched_wells.size(), 0.0 );
    std::transform( sched_wells.begin(), sched_wells.end(), iwel.begin(), getctrl );
    return iwel;
}

std::vector< double > serialize_OPM_XWEL( const data::Wells& wells,
                                          int sim_step,
                                          const std::vector< const Well* >& sched_wells,
                                          const Phases& phase_spec,
                                          const EclipseGrid& grid ) {

    using rt = data::Rates::opt;

    std::vector< rt > phases;
    if( phase_spec.active( Phase::WATER ) ) phases.push_back( rt::wat );
    if( phase_spec.active( Phase::OIL ) )   phases.push_back( rt::oil );
    if( phase_spec.active( Phase::GAS ) )   phases.push_back( rt::gas );

    std::vector< double > xwel;
    for( const auto* sched_well : sched_wells ) {

        if( wells.count( sched_well->name() ) == 0 || sched_well->getStatus(sim_step) == Opm::WellCommon::SHUT) {
            const auto elems = (sched_well->getConnections( sim_step ).size()
                               * (phases.size() + data::Connection::restart_size))
                + 2 /* bhp, temperature */
                + phases.size();

            // write zeros if no well data is provided or it is shut
            xwel.insert( xwel.end(), elems, 0.0 );
            continue;
        }

        const auto& well = wells.at( sched_well->name() );

        xwel.push_back( well.bhp );
        xwel.push_back( well.temperature );
        for( auto phase : phases )
            xwel.push_back( well.rates.get( phase ) );

        for( const auto& sc : sched_well->getConnections( sim_step ) ) {
            const auto i = sc.getI(), j = sc.getJ(), k = sc.getK();

            const auto rs_size = phases.size() + data::Connection::restart_size;
            if( !grid.cellActive( i, j, k ) || sc.state == WellCompletion::SHUT ) {
                xwel.insert( xwel.end(), rs_size, 0.0 );
                continue;
            }

            const auto active_index = grid.activeIndex( i, j, k );
            const auto at_index = [=]( const data::Connection& c ) {
                return c.index == active_index;
            };
            const auto& connection = std::find_if( well.connections.begin(),
                                                   well.connections.end(),
                                                   at_index );

            if( connection == well.connections.end() ) {
                xwel.insert( xwel.end(), rs_size, 0.0 );
                continue;
            }

            xwel.push_back( connection->pressure );
            xwel.push_back( connection->reservoir_rate );
            for( auto phase : phases )
                xwel.push_back( connection->rates.get( phase ) );
        }
    }

    return xwel;
}


std::vector<const char*> serialize_ZWEL( const std::vector<const Well *>& wells) {
    std::vector<const char*> data( wells.size( ) * NZWELZ , "");
    size_t offset = 0;

    for (const auto& well : wells) {
        data[ offset ] = well->name().c_str();
        offset += NZWELZ;
    }
    return data;
}





template< typename T >
void write_kw(ecl_rst_file_type * rst_file , ERT::EclKW< T >&& kw) {
    ecl_rst_file_add_kw( rst_file, kw.get() );
}

void writeHeader(ecl_rst_file_type * rst_file,
                 int sim_step,
                 int report_step,
                 time_t posix_time,
                 double sim_days,
                 int ert_phase_mask,
                 const UnitSystem& units,
                 const Schedule& schedule,
                 const EclipseGrid& grid) {

    ecl_rsthead_type rsthead_data = {};

    rsthead_data.sim_time    = posix_time;
    rsthead_data.nactive     = grid.getNumActive();
    rsthead_data.nx          = grid.getNX();
    rsthead_data.ny          = grid.getNY();
    rsthead_data.nz          = grid.getNZ();
    rsthead_data.nwells      = schedule.numWells(sim_step);
    rsthead_data.niwelz      = NIWELZ;
    rsthead_data.nzwelz      = NZWELZ;
    rsthead_data.niconz      = NICONZ;
    rsthead_data.ncwmax      = schedule.getMaxNumConnectionsForWells(sim_step);
    rsthead_data.phase_sum   = ert_phase_mask;
    rsthead_data.sim_days    = sim_days;
    rsthead_data.unit_system = units.getEclType( );

    ecl_util_set_date_values( rsthead_data.sim_time,
                              &rsthead_data.day,
                              &rsthead_data.month,
                              &rsthead_data.year );

    ecl_rst_file_fwrite_header( rst_file, report_step , &rsthead_data );
}

  ERT::ert_unique_ptr< ecl_kw_type, ecl_kw_free > ecl_kw( const std::string& kw, const std::vector<double>& data, bool write_double) {
      ERT::ert_unique_ptr< ecl_kw_type, ecl_kw_free > kw_ptr;

      if (write_double) {
          ecl_kw_type * ecl_kw = ecl_kw_alloc( kw.c_str() , data.size() , ECL_DOUBLE );
          ecl_kw_set_memcpy_data( ecl_kw , data.data() );
          kw_ptr.reset( ecl_kw );
      } else {
          ecl_kw_type * ecl_kw = ecl_kw_alloc( kw.c_str() , data.size() , ECL_FLOAT );
          float * float_data = ecl_kw_get_float_ptr( ecl_kw );
          for (size_t i=0; i < data.size(); i++)
              float_data[i] = data[i];
          kw_ptr.reset( ecl_kw );
      }

      return kw_ptr;
  }



  void writeSolution(ecl_rst_file_type* rst_file, const data::Solution& solution, bool write_double) {
    ecl_rst_file_start_solution( rst_file );
    for (const auto& elm: solution) {
        if (elm.second.target == data::TargetType::RESTART_SOLUTION)
            ecl_rst_file_add_kw( rst_file , ecl_kw(elm.first, elm.second.data, write_double).get());
     }
     ecl_rst_file_end_solution( rst_file );

     for (const auto& elm: solution) {
        if (elm.second.target == data::TargetType::RESTART_AUXILIARY)
            ecl_rst_file_add_kw( rst_file , ecl_kw(elm.first, elm.second.data, write_double).get());
     }
  }


  void writeExtraData(ecl_rst_file_type* rst_file, const RestartValue::ExtraVector& extra_data) {
    for (const auto& extra_value : extra_data) {
        const std::string& key = extra_value.first.key;
        const std::vector<double>& data = extra_value.second;
        {
            ecl_kw_type * ecl_kw = ecl_kw_alloc_new_shared( key.c_str() , data.size() , ECL_DOUBLE , const_cast<double *>(data.data()));
            ecl_rst_file_add_kw( rst_file , ecl_kw);
            ecl_kw_free( ecl_kw );
        }
    }
}



void writeWell(ecl_rst_file_type* rst_file, int sim_step, const EclipseState& es , const EclipseGrid& grid, const Schedule& schedule, const data::Wells& wells) {
    const auto sched_wells  = schedule.getWells(sim_step);
    const auto& phases = es.runspec().phases();
    const size_t ncwmax = schedule.getMaxNumConnectionsForWells(sim_step);

    const auto opm_xwel  = serialize_OPM_XWEL( wells, sim_step, sched_wells, phases, grid );
    const auto opm_iwel  = serialize_OPM_IWEL( wells, sched_wells );
    const auto iwel_data = serialize_IWEL(sim_step, sched_wells, grid);
    const auto icon_data = serialize_ICON(sim_step , ncwmax, sched_wells, grid);
    const auto zwel_data = serialize_ZWEL( sched_wells );

    write_kw( rst_file, ERT::EclKW< int >( IWEL_KW, iwel_data) );
    write_kw( rst_file, ERT::EclKW< const char* >(ZWEL_KW, zwel_data ) );
    write_kw( rst_file, ERT::EclKW< double >( OPM_XWEL, opm_xwel ) );
    write_kw( rst_file, ERT::EclKW< int >( OPM_IWEL, opm_iwel ) );
    write_kw( rst_file, ERT::EclKW< int >( ICON_KW, icon_data ) );
}

void checkSaveArguments(const EclipseState& es,
                        const RestartValue& restart_value,
                        const EclipseGrid& grid) {

  for (const auto& elm: restart_value.solution)
    if (elm.second.data.size() != grid.getNumActive())
      throw std::runtime_error("Wrong size on solution vector: " + elm.first);


  if (es.getSimulationConfig().getThresholdPressure().size() > 0) {
      // If the the THPRES option is active the restart_value should have a
      // THPRES field. This is not enforced here because not all the opm
      // simulators have been updated to include the THPRES values.
      if (!restart_value.hasExtra("THPRES")) {
          OpmLog::warning("This model has THPRES active - should have THPRES as part of restart data.");
          return;
      }

      size_t num_regions = es.getTableManager().getEqldims().getNumEquilRegions();
      const auto& thpres = restart_value.getExtra("THPRES");
      if (thpres.size() != num_regions * num_regions)
          throw std::runtime_error("THPRES vector has invalid size - should have num_region * num_regions.");
  }
}
} // Anonymous namespace


void save(const std::string& filename,
          int report_step,
          double seconds_elapsed,
          RestartValue value,
          const EclipseState& es,
          const EclipseGrid& grid,
          const Schedule& schedule,
          bool write_double)
{
    checkSaveArguments(es, value, grid);
    {
        int sim_step = std::max(report_step - 1, 0);
        int ert_phase_mask = es.runspec().eclPhaseMask( );
        const auto& units = es.getUnits();
        time_t posix_time = schedule.posixStartTime() + seconds_elapsed;
        const auto sim_time = units.from_si( UnitSystem::measure::time, seconds_elapsed );
        ERT::ert_unique_ptr< ecl_rst_file_type, ecl_rst_file_close > rst_file;

        if (ERT::EclFiletype( filename ) == ECL_UNIFIED_RESTART_FILE)
            rst_file.reset( ecl_rst_file_open_write_seek( filename.c_str(), report_step ) );
        else
            rst_file.reset( ecl_rst_file_open_write( filename.c_str() ) );

        // Convert solution fields and extra values from SI to user units.
        value.solution.convertFromSI(units);
        for (auto & extra_value : value.extra) {
            const auto& restart_key = extra_value.first;
            auto & data = extra_value.second;

            units.from_si(restart_key.dim, data);
        }

        writeHeader( rst_file.get(), sim_step, report_step, posix_time , sim_time, ert_phase_mask, units, schedule , grid );
        writeWell( rst_file.get(), sim_step, es , grid, schedule, value.wells);
        writeSolution( rst_file.get(), value.solution, write_double );
        writeExtraData( rst_file.get(), value.extra );
    }
}
}
}
