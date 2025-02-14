/*
 * Copyright 2021 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_SYSTEM_TFS
#ifndef __CUDACC__

#include <cmath>
#include <vector>

#include "GeoRaster.hpp"
#include "Shared/Utilities.hpp"

// Allow input types to GeoRaster that are different than class types/output Z type
// So we can move everything to the type of T and Z (which can each be either float
// or double)
template <typename T, typename Z>
template <typename T2, typename Z2>
GeoRaster<T, Z>::GeoRaster(const Column<T2>& input_x,
                           const Column<T2>& input_y,
                           const Column<Z2>& input_z,
                           const double bin_dim_meters,
                           const bool geographic_coords,
                           const bool align_bins_to_zero_based_grid)
    : bin_dim_meters_(bin_dim_meters)
    , geographic_coords_(geographic_coords)
    , null_sentinel_(std::numeric_limits<Z>::lowest()) {
  const int64_t input_size{input_z.size()};
  if (input_size <= 0) {
    num_bins_ = 0;
    num_x_bins_ = 0;
    num_y_bins_ = 0;
    return;
  }
  const auto min_max_x = get_column_min_max(input_x);
  const auto min_max_y = get_column_min_max(input_y);
  x_min_ = min_max_x.first;
  x_max_ = min_max_x.second;
  y_min_ = min_max_y.first;
  y_max_ = min_max_y.second;

  if (align_bins_to_zero_based_grid && !geographic_coords_) {
    // For implicit, data-defined bounds, we treat the max of the x and y ranges as
    // inclusive (closed interval), since if the max of the data in either x/y dimensions
    // is at the first value of the next bin, values at that max will be discarded if we
    // don't include the final bin. For exmaple, if the input data (perhaps already binned
    // with a group by query) goes from 0.0 to 40.0 in both x and y directions, we should
    // have the last x/y bins cover the range [40.0, 50.0), not [30.0, 40.0)
    align_bins_max_inclusive();
  }
  // std::cout << "X range: [" << x_min_ << ", " << x_max_ << "]" << std::endl;
  // std::cout << "Y range: [" << y_min_ << ", " << y_max_ << "]" << std::endl;

  calculate_bins_and_scales();
  compute(input_x, input_y, input_z);
}

// Allow input types to GeoRaster that are different than class types/output Z type
// So we can move everything to the type of T and Z (which can each be either float
// or double)
template <typename T, typename Z>
template <typename T2, typename Z2>
GeoRaster<T, Z>::GeoRaster(const Column<T2>& input_x,
                           const Column<T2>& input_y,
                           const Column<Z2>& input_z,
                           const double bin_dim_meters,
                           const bool geographic_coords,
                           const bool align_bins_to_zero_based_grid,
                           const T x_min,
                           const T x_max,
                           const T y_min,
                           const T y_max)
    : bin_dim_meters_(bin_dim_meters)
    , geographic_coords_(geographic_coords)
    , null_sentinel_(std::numeric_limits<Z>::lowest())
    , x_min_(x_min)
    , x_max_(x_max)
    , y_min_(y_min)
    , y_max_(y_max) {
  if (align_bins_to_zero_based_grid && !geographic_coords_) {
    // For explicit, user-defined bounds, we treat the max of the x and y ranges as
    // exclusive (open interval), since if the user specifies the max x/y as the end of
    // the bin, they do not intend to add the next full bin For example, if a user
    // specifies a bin_dim_meters of 10.0 and an x and y range from 0 to 40.0, they almost
    // assuredly intend for there to be 4 bins in each of the x and y dimensions, with the
    // last bin of range [30.0, 40.0), not 5 with the final bin's range from [40.0, 50.0)
    align_bins_max_exclusive();
  }
  calculate_bins_and_scales();
  compute(input_x, input_y, input_z);
}

template <typename T, typename Z>
inline Z GeoRaster<T, Z>::offset_source_z_from_raster_z(const int64_t source_x_bin,
                                                        const int64_t source_y_bin,
                                                        const Z source_z_offset) const {
  if (is_bin_out_of_bounds(source_x_bin, source_y_bin)) {
    return null_sentinel_;
  }
  const Z terrain_z = z_[x_y_bin_to_bin_index(source_x_bin, source_y_bin, num_x_bins_)];
  if (terrain_z == null_sentinel_) {
    return terrain_z;
  }
  return terrain_z + source_z_offset;
}

template <typename T, typename Z>
inline Z GeoRaster<T, Z>::fill_bin_from_avg_neighbors(const int64_t x_centroid_bin,
                                                      const int64_t y_centroid_bin,
                                                      const int64_t bins_radius) const {
  T val = 0.0;
  int32_t count = 0;
  for (int64_t y_bin = y_centroid_bin - bins_radius;
       y_bin <= y_centroid_bin + bins_radius;
       y_bin++) {
    for (int64_t x_bin = x_centroid_bin - bins_radius;
         x_bin <= x_centroid_bin + bins_radius;
         x_bin++) {
      if (x_bin >= 0 && x_bin < num_x_bins_ && y_bin >= 0 && y_bin < num_y_bins_) {
        const int64_t bin_idx = x_y_bin_to_bin_index(x_bin, y_bin, num_x_bins_);
        const Z bin_val = z_[bin_idx];
        if (bin_val != null_sentinel_) {
          count++;
          val += bin_val;
        }
      }
    }
  }
  return (count == 0) ? null_sentinel_ : val / count;
}

template <typename T, typename Z>
void GeoRaster<T, Z>::align_bins_max_inclusive() {
  x_min_ = std::floor(x_min_ / bin_dim_meters_) * bin_dim_meters_;
  x_max_ = std::floor(x_max_ / bin_dim_meters_) * bin_dim_meters_ +
           bin_dim_meters_;  // Snap to end of bin
  y_min_ = std::floor(y_min_ / bin_dim_meters_) * bin_dim_meters_;
  y_max_ = std::floor(y_max_ / bin_dim_meters_) * bin_dim_meters_ +
           bin_dim_meters_;  // Snap to end of bin
}

template <typename T, typename Z>
void GeoRaster<T, Z>::align_bins_max_exclusive() {
  x_min_ = std::floor(x_min_ / bin_dim_meters_) * bin_dim_meters_;
  x_max_ = std::ceil(x_max_ / bin_dim_meters_) * bin_dim_meters_;
  y_min_ = std::floor(y_min_ / bin_dim_meters_) * bin_dim_meters_;
  y_max_ = std::ceil(y_max_ / bin_dim_meters_) * bin_dim_meters_;
}

template <typename T, typename Z>
void GeoRaster<T, Z>::calculate_bins_and_scales() {
  x_range_ = x_max_ - x_min_;
  y_range_ = y_max_ - y_min_;
  if (geographic_coords_) {
    const T x_centroid = (x_min_ + x_max_) * 0.5;
    const T y_centroid = (y_min_ + y_max_) * 0.5;
    x_meters_per_degree_ =
        distance_in_meters(x_min_, y_centroid, x_max_, y_centroid) / x_range_;

    y_meters_per_degree_ =
        distance_in_meters(x_centroid, y_min_, x_centroid, y_max_) / y_range_;

    num_x_bins_ = x_range_ * x_meters_per_degree_ / bin_dim_meters_;
    num_y_bins_ = y_range_ * y_meters_per_degree_ / bin_dim_meters_;

    x_scale_input_to_bin_ = x_meters_per_degree_ / bin_dim_meters_;
    y_scale_input_to_bin_ = y_meters_per_degree_ / bin_dim_meters_;
    x_scale_bin_to_input_ = bin_dim_meters_ / x_meters_per_degree_;
    y_scale_bin_to_input_ = bin_dim_meters_ / y_meters_per_degree_;

  } else {
    num_x_bins_ = x_range_ / bin_dim_meters_;
    num_y_bins_ = y_range_ / bin_dim_meters_;

    x_scale_input_to_bin_ = 1.0 / bin_dim_meters_;
    y_scale_input_to_bin_ = 1.0 / bin_dim_meters_;
    x_scale_bin_to_input_ = bin_dim_meters_;
    y_scale_bin_to_input_ = bin_dim_meters_;
  }
  num_bins_ = num_x_bins_ * num_y_bins_;
}

template <typename T, typename Z>
template <typename T2, typename Z2>
void GeoRaster<T, Z>::compute(const Column<T2>& input_x,
                              const Column<T2>& input_y,
                              const Column<Z2>& input_z) {
  const int64_t input_size{input_z.size()};
  z_.resize(num_bins_, null_sentinel_);
  for (int64_t sparse_idx = 0; sparse_idx != input_size; ++sparse_idx) {
    const int64_t x_bin = get_x_bin(input_x[sparse_idx]);
    const int64_t y_bin = get_y_bin(input_y[sparse_idx]);
    if (x_bin < 0 || x_bin >= num_x_bins_ || y_bin < 0 || y_bin >= num_y_bins_) {
      continue;
    }
    // Take the max height for this version, but may want to allow different metrics
    // like average as well
    const int64_t bin_idx = x_y_bin_to_bin_index(x_bin, y_bin, num_x_bins_);
    if (!(input_z.isNull(sparse_idx)) && input_z[sparse_idx] > z_[bin_idx]) {
      z_[bin_idx] = input_z[sparse_idx];
    }
  }
}

template <typename T, typename Z>
int64_t GeoRaster<T, Z>::outputDenseColumns(
    TableFunctionManager& mgr,
    Column<T>& output_x,
    Column<T>& output_y,
    Column<Z>& output_z,
    const int64_t neighborhood_null_fill_radius) const {
  mgr.set_output_row_size(num_bins_);
  for (int64_t y_bin = 0; y_bin < num_y_bins_; ++y_bin) {
    for (int64_t x_bin = 0; x_bin < num_x_bins_; ++x_bin) {
      const int64_t bin_idx = x_y_bin_to_bin_index(x_bin, y_bin, num_x_bins_);
      output_x[bin_idx] = x_min_ + (x_bin + 0.5) * x_scale_bin_to_input_;
      output_y[bin_idx] = y_min_ + (y_bin + 0.5) * y_scale_bin_to_input_;
      const Z z_val = z_[bin_idx];
      if (z_val == null_sentinel_) {
        output_z.setNull(bin_idx);
        if (neighborhood_null_fill_radius) {
          const Z avg_neighbor_value =
              fill_bin_from_avg_neighbors(x_bin, y_bin, neighborhood_null_fill_radius);
          if (avg_neighbor_value != null_sentinel_) {
            output_z[bin_idx] = avg_neighbor_value;
          }
        }
      } else {
        output_z[bin_idx] = z_[bin_idx];
      }
    }
  }
  return num_bins_;
}

#endif  // __CUDACC__
#endif  // HAVE_SYSTEM_TFS
