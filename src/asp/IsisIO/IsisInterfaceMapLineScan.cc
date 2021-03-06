// __BEGIN_LICENSE__
// Copyright (C) 2006-2011 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


// ASP
#include <vw/Cartography/SimplePointImageManipulation.h>
#include <vw/Camera/CameraModel.h>
#include <vw/Math/LevenbergMarquardt.h>
#include <asp/IsisIO/IsisInterfaceMapLineScan.h>

// ISIS
#include <ProjectionFactory.h>
#include <iTime.h>
#include <Latitude.h>
#include <Longitude.h>

using namespace vw;
using namespace asp;
using namespace asp::isis;

// Constructor
IsisInterfaceMapLineScan::IsisInterfaceMapLineScan( std::string const& filename ) :
  IsisInterface( filename ), m_projection( Isis::ProjectionFactory::CreateFromCube(m_label) ) {

  // Gutting Isis::Camera
  m_distortmap = m_camera->DistortionMap();
  m_groundmap = m_camera->GroundMap();
  m_focalmap = m_camera->FocalPlaneMap();
  m_cache_px[0] = m_cache_px[1] = std::numeric_limits<double>::quiet_NaN();
}

// Custom Functions
class EphemerisLMA : public vw::math::LeastSquaresModelBase<EphemerisLMA> {
  vw::Vector3 m_point;
  Isis::Camera* m_camera;
  Isis::CameraDistortionMap *m_distortmap;
  Isis::CameraFocalPlaneMap *m_focalmap;
public:
  typedef vw::Vector<double> result_type; // Back project result
      typedef vw::Vector<double> domain_type; // Ephemeris time
  typedef vw::Matrix<double> jacobian_type;

  inline EphemerisLMA( vw::Vector3 const& point,
                       Isis::Camera* camera,
                       Isis::CameraDistortionMap* distortmap,
                       Isis::CameraFocalPlaneMap* focalmap ) : m_point(point), m_camera(camera), m_distortmap(distortmap), m_focalmap(focalmap) {}

  inline result_type operator()( domain_type const& x ) const;
};


// LMA for projecting point to linescan camera
EphemerisLMA::result_type
EphemerisLMA::operator()( EphemerisLMA::domain_type const& x ) const {

  // Setting Ephemeris Time
  m_camera->SetTime( Isis::iTime( x[0] ));

  // Calculating the look direction in camera frame
  Vector3 instru;
  m_camera->InstrumentPosition(&instru[0]);
  instru *= 1000;  // Spice gives in km
  Vector3 lookB = normalize( m_point - instru );
  std::vector<double> lookB_copy(3);
  std::copy(lookB.begin(),lookB.end(),lookB_copy.begin());
  std::vector<double> lookJ = m_camera->BodyRotation()->J2000Vector(lookB_copy);
  std::vector<double> lookC = m_camera->InstrumentRotation()->ReferenceVector(lookJ);
  Vector3 look;
  std::copy(lookC.begin(),lookC.end(),look.begin());

  // Projecting to mm focal plane
  look = m_camera->FocalLength() * (look / look[2]);
  m_distortmap->SetUndistortedFocalPlane(look[0], look[1]);
  m_focalmap->SetFocalPlane( m_distortmap->FocalPlaneX(),
                             m_distortmap->FocalPlaneY() );
  result_type result(1);
  // Not exactly sure about lineoffset .. but ISIS does it
  result[0] = m_focalmap->DetectorLineOffset() - m_focalmap->DetectorLine();

  return result;
}

Vector2
IsisInterfaceMapLineScan::point_to_pixel( Vector3 const& point ) const {

  // First seed LMA with an ephemeris time in the middle of the image
  double middle_et =
    m_camera->CacheStartTime().Et() + (m_camera->CacheEndTime().Et()-m_camera->CacheStartTime().Et())/2.0;

  // Build LMA
  EphemerisLMA model( point, m_camera.get(), m_distortmap, m_focalmap );
  int status;
  Vector<double> objective(1), start(1);
  start[0] = middle_et;
  Vector<double> solution_e = math::levenberg_marquardt( model,
                                                         start,
                                                         objective,
                                                         status );

  // Make sure we found ideal time
  VW_ASSERT( status > 0,
             camera::PointToPixelErr() << " Unable to project point into linescan camera " );

  // Setting to camera time to solution
  m_camera->SetTime( Isis::iTime( solution_e[0] ) );

  // Working out pointing
  Vector3 center;
  m_camera->InstrumentPosition(&center[0]);
  Vector3 look = normalize(point-1000*center);

  // Calculating Rotation to camera frame
  std::vector<double> rot_inst = m_camera->InstrumentRotation()->Matrix();
  std::vector<double> rot_body = m_camera->BodyRotation()->Matrix();
  MatrixProxy<double,3,3> R_inst(&(rot_inst[0]));
  MatrixProxy<double,3,3> R_body(&(rot_body[0]));
  look = transpose(R_body*transpose(R_inst))*look;
  look = m_camera->FocalLength() * (look / look[2]);

  // Projecting back on to ground to find out time
  m_groundmap->SetFocalPlane( look[0],
                              look[1],
                              look[2] );

  m_projection->SetGround( m_camera->UniversalLatitude(),
                           m_camera->UniversalLongitude() );
  m_cache_px = Vector2( m_projection->WorldX()-1,
                        m_projection->WorldY()-1 );
  return m_cache_px;
}

Vector3
IsisInterfaceMapLineScan::camera_center( Vector2 const& px ) const {
  if ( px != m_cache_px ) {
    m_cache_px = px;
    if (!m_projection->SetWorld( px[0]+1,
                                 px[1]+1 ))
      vw_throw( camera::PixelToRayErr() << "Failed to SetWorld." );
    if (!m_groundmap->SetGround( Isis::Latitude(m_projection->UniversalLatitude(),Isis::Angle::Degrees),
                                 Isis::Longitude(m_projection->UniversalLongitude(),Isis::Angle::Degrees) ) )
      vw_throw( camera::PixelToRayErr() << "Failed to SetGround." );
  }
  Vector3 position;
  m_camera->InstrumentPosition( &position[0] );
  return position * 1e3;
}

Vector3
IsisInterfaceMapLineScan::pixel_to_vector( Vector2 const& px ) const {
  Vector3 sB = camera_center( px );
  Vector3 p_pB;
  m_camera->Sensor::Coordinate( &p_pB[0] );
  return normalize(p_pB*1000 - sB);
}

Quat
IsisInterfaceMapLineScan::camera_pose( Vector2 const& px ) const {
  camera_center( px );
  std::vector<double> rot_inst = m_camera->InstrumentRotation()->Matrix();
  std::vector<double> rot_body = m_camera->BodyRotation()->Matrix();
  MatrixProxy<double,3,3> R_inst(&(rot_inst[0]));
  MatrixProxy<double,3,3> R_body(&(rot_body[0]));
  return Quat(R_body*transpose(R_inst));
}
