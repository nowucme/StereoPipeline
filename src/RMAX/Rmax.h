#ifndef RMAX_H__
#define RMAX_H__

#include <string>
#include <vw/FileIO/DiskImageResourcePNG.h>
#include <vw/Camera/CAHVORModel.h>
#include <vw/Camera/BundleAdjust.h>
#include <vw/Core.h>

using namespace vw; 

// Split filename into base and extension.
int split_filename(const std::string& filename, std::string& base, std::string& extension);

struct ImageInfo {
  std::string filename;
  double easting, northing;
  double heading, pitch, roll;
  double height, tilt;
  enum {
    LEFT=1, RIGHT=2, COLOR=3
  } camera;
};

void read_image_info( std::string const& filename, ImageInfo& info );

vw::camera::CAHVORModel rmax_image_camera_model( ImageInfo const& info,
                                                 vw::Vector3 const& position_correction,
                                                 vw::Vector3 const& pose_correction);

vw::camera::CAHVORModel rmax_image_camera_model( ImageInfo const& info );

vw::camera::CAHVORModel rmax_image_camera_model( std::string const& filename );

bool may_overlap( ImageInfo const& i1, ImageInfo const& i2 );
bool may_overlap( std::string const& file1, std::string const& file2 );

// -----

class HelicopterBundleAdjustmentModel : public camera::BundleAdjustmentModelBase<HelicopterBundleAdjustmentModel, 6, 3> {
  std::vector<ImageInfo> m_image_infos;
  std::vector<Vector<double,6> > m_adjustments;
  
public:
  typedef camera::AdjustedCameraModel camera_type;

  HelicopterBundleAdjustmentModel(std::vector<ImageInfo> const& image_infos) : 
    m_image_infos(image_infos), m_adjustments(image_infos.size()) {}

  unsigned size() const { return m_image_infos.size(); }

  void update(std::vector<Vector<double, camera_params_n> > a) {
    m_adjustments = a;
  }
   
  void write_adjustment(int j, std::string const& filename) {
    std::ofstream ostr(filename.c_str());
    ostr << m_adjustments[j][0] << " " << m_adjustments[j][1] << " " << m_adjustments[j][2] << "\n";
    ostr << m_adjustments[j][3] << " " << m_adjustments[j][4] << " " << m_adjustments[j][5] << "\n";
  }

  void write_adjusted_camera(int j, std::string const& filename) {
    Vector<double,6> a_j = m_adjustments[j];
    Vector3 position_correction = subvector(a_j, 0, 3);
    Vector3 pose_correction = subvector(a_j, 3,3);
    camera::CAHVORModel cam = rmax_image_camera_model(m_image_infos[j], position_correction, pose_correction);
    cam.write(filename);
  }

  std::vector<boost::shared_ptr<camera::CameraModel> > adjusted_cameras() {
    std::vector<boost::shared_ptr<camera::CameraModel> > result(m_adjustments.size());
    for (unsigned j = 0; j < result.size(); ++j) {
      Vector3 position_correction = subvector(m_adjustments[j], 0, 3);
      Vector3 pose_correction = subvector(m_adjustments[j], 3,3);
      
      camera::CAHVORModel* cahvor = new camera::CAHVORModel;
      *cahvor = rmax_image_camera_model(m_image_infos[j], position_correction, pose_correction);

      result[j] = boost::shared_ptr<camera::CameraModel>( cahvor );
    }
    return result;
  }
  
  // Given the 'a' vector (camera model parameters) for the j'th
  // image, and the 'b' vector (3D point location) for the i'th
  // point, return the location of b_i on imager j in pixel
  // coordinates.
  Vector2 operator() ( unsigned i, unsigned j, Vector<double,6> const& a_j, Vector<double,3> const& b_i ) {
    Vector3 position_correction = subvector(a_j, 0, 3);
    Vector3 pose_correction = subvector(a_j, 3,3);
    camera::CAHVORModel cam = rmax_image_camera_model(m_image_infos[j], position_correction, pose_correction);
    return cam.point_to_pixel(b_i);
  }    
  
  Vector<double,6> initial_parameters(unsigned j) const { return Vector<double,6>(); }
};


#endif // __RMAX_H__