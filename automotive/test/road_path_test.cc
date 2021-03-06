#include "drake/automotive/road_path.h"

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "drake/automotive/maliput/api/lane.h"
#include "drake/automotive/maliput/api/lane_data.h"
#include "drake/automotive/maliput/api/road_geometry.h"
#include "drake/automotive/maliput/dragway/road_geometry.h"
#include "drake/automotive/maliput/multilane/builder.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"

namespace drake {
namespace automotive {
namespace {

using maliput::api::GeoPosition;
using maliput::api::HBounds;
using maliput::api::JunctionId;
using maliput::api::Lane;
using maliput::api::LaneEnd;
using maliput::api::LanePosition;
using maliput::api::RoadGeometry;

using maliput::multilane::ArcOffset;
using maliput::multilane::Builder;
using maliput::multilane::BuilderFactory;
using maliput::multilane::ComputationPolicy;
using maliput::multilane::Direction;
using maliput::multilane::Endpoint;
using maliput::multilane::EndpointZ;
using maliput::multilane::LaneLayout;
using maliput::multilane::LineOffset;
using maliput::multilane::EndReference;
using maliput::multilane::StartReference;

// The length of the straight lane segment.
const double kStraightRoadLength{10};

// The arc radius, angular displacement, and length of the curved road segment.
const double kCurvedRoadRadius{10};
const double kCurvedRoadTheta{M_PI_2};
const double kCurvedRoadLength{kCurvedRoadRadius * M_PI_2};

const double kTotalRoadLength{kStraightRoadLength + kCurvedRoadLength};
const EndpointZ kEndZ{0, 0, 0, 0};  // Specifies zero elevation/super-elevation.

// Build a road with two lanes in series.
std::unique_ptr<const RoadGeometry> MakeTwoLaneRoad(bool is_opposing) {
  auto builder = BuilderFactory().Make(
      4. /* lane width */, HBounds(0., 5.), 0.01 /* linear tolerance */,
      M_PI_2 / 180.0 /* angular tolerance */, 1. /* scale length*/,
      ComputationPolicy::kPreferAccuracy /* accuracy */);
  const LaneLayout lane_layout(
      2. /* left shoulder */, 2. /* right shoulder */, 1 /* number of lanes */,
      0 /* reference lane*/, 0. /* reference r0 */);

  builder->Connect(
      "0_fwd", lane_layout,
      StartReference().at(Endpoint({0, 0, 0}, kEndZ), Direction::kForward),
      LineOffset(kStraightRoadLength),
      EndReference().z_at(kEndZ, Direction::kForward));

  if (is_opposing) {
    // Construct a curved segment that is directionally opposite the straight
    // lane.
    builder->Connect(
        "1_rev", lane_layout,
        StartReference().at(Endpoint({kStraightRoadLength + kCurvedRoadRadius,
                                      kCurvedRoadRadius, 1.5 * M_PI},
                                     kEndZ),
                            Direction::kForward),
        ArcOffset(kCurvedRoadRadius, -kCurvedRoadTheta),
        EndReference().z_at(kEndZ, Direction::kForward));
  } else {
    // Construct a curved segment that is directionally confluent with the
    // straight lane.
    builder->Connect(
        "1_fwd", lane_layout,
        StartReference().at(Endpoint({kStraightRoadLength, 0, 0}, kEndZ),
                            Direction::kForward),
        ArcOffset(kCurvedRoadRadius, kCurvedRoadTheta),
        EndReference().z_at(kEndZ, Direction::kForward));
  }

  return builder->Build(maliput::api::RoadGeometryId("TwoLaneStretchOfRoad"));
}

const Lane* GetLaneById(const RoadGeometry& road, const std::string& lane_id) {
  for (int i = 0; i < road.num_junctions(); ++i) {
    if (road.junction(i)->id() == JunctionId(lane_id)) {
      return road.junction(i)->segment(0)->lane(0);
    }
  }
  throw std::runtime_error("No matching junction name in the road network");
}

static double path_radius(const Vector3<double> value) {
  Vector3<double> result;
  result << value(0) - kStraightRoadLength, value(1) - kCurvedRoadRadius,
      value(2);
  return result.norm();
}

// Tests the constructor given a sufficient number of points.
GTEST_TEST(IdmControllerTest, ConstructOpposingSegments) {
  const double kStepSize{0.5};
  // Instantiate a road with opposing segments.
  auto road_opposing = MakeTwoLaneRoad(true);
  // Start in the straight segment and progress in the positive-s-direction.
  const LaneDirection initial_lane_dir =
      LaneDirection(GetLaneById(*road_opposing, "j:0_fwd"), /* lane */
                    true);                                  /* with_s */
  // Create a finely-discretized path with a sufficient number of segments to
  // cover the full length.
  const auto path =
      RoadPath<double>(initial_lane_dir, /* initial_lane_direction */
                       kStepSize,        /* step_size */
                       100);             /* num_breaks */
  ASSERT_LE(kTotalRoadLength,
            path.get_path().end_time() - path.get_path().start_time());

  // Expect the lane boundary values to match.
  Vector3<double> expected_value{};
  Vector3<double> actual_value{};
  expected_value << 0., 0., 0.;
  actual_value = path.get_path().value(0.);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));
  // N.B. Using tolerance of 1e-3 to account for possible interpolation errors.

  // Derive s-position of the straight road segment from the number of break
  // point steps taken to reach  kStraightRoadLength from the end of the road.
  const double straight_length{
      path.get_path().start_time(std::ceil(kStraightRoadLength / kStepSize))};
  expected_value << 10., 0., 0.;
  actual_value = path.get_path().value(straight_length);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));

  const double total_length{path.get_path().end_time()};
  expected_value << 20., 10., 0.;
  actual_value = path.get_path().value(total_length);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));

  // Pick a few arbitrary points on the curved section, expect them to trace the
  // arc, hence demonstrating the interpolation is working.
  actual_value = path.get_path().value(4. / 7. * kTotalRoadLength);
  EXPECT_NEAR(kCurvedRoadRadius, path_radius(actual_value), 1e-3);

  actual_value = path.get_path().value(5. / 7. * kTotalRoadLength);
  EXPECT_NEAR(kCurvedRoadRadius, path_radius(actual_value), 1e-3);

  actual_value = path.get_path().value(6. / 7. * kTotalRoadLength);
  EXPECT_NEAR(kCurvedRoadRadius, path_radius(actual_value), 1e-3);

  // Check that the number of segments created is well below the max
  // number specified.
  EXPECT_GT(1000, path.get_path().get_number_of_segments());
}

GTEST_TEST(IdmControllerTest, ConstructConfluentSegments) {
  const double kStepSize{0.5};
  // Instantiate a road with confluent segments.
  auto road_confluent = MakeTwoLaneRoad(false);
  // Start in the curved segment, and progress in the negative-s-direction.
  const LaneDirection initial_lane_dir =
      LaneDirection(GetLaneById(*road_confluent, "j:1_fwd"), /* lane */
                    false);                                  /* with_s */
  // Create a finely-discretized path with a sufficient number of segments to
  // cover the full length.
  const auto path =
      RoadPath<double>(initial_lane_dir, /* initial_lane_direction */
                       kStepSize,        /* step_size */
                       100);             /* num_breaks */
  ASSERT_LE(kTotalRoadLength,
            path.get_path().end_time() - path.get_path().start_time());

  // Expect the lane boundary values to match.
  Vector3<double> expected_value{};
  Vector3<double> actual_value{};
  expected_value << 20., 10., 0.;
  actual_value = path.get_path().value(0.);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));

  double total_length{path.get_path().end_time()};
  // Derive s-position of the straight road segment from the number of break
  // point steps taken to reach kStraightRoadLength from the start of the road.
  double straight_length{
      path.get_path().end_time(std::ceil(kStraightRoadLength / kStepSize))};
  double curved_length{total_length - straight_length};
  expected_value << 10., 0., 0.;
  actual_value = path.get_path().value(curved_length);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));

  expected_value << 0., 0., 0.;
  actual_value = path.get_path().value(total_length);
  EXPECT_TRUE(CompareMatrices(expected_value, actual_value, 1e-3));
}

}  // namespace
}  // namespace automotive
}  // namespace drake
