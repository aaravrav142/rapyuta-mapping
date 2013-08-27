#include <keyframe_map.h>
#include <boost/filesystem.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <fstream>
#include <reduce_jacobian_rgb.h>
#include <reduce_jacobian_rgb_3d.h>

#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/nonfree/nonfree.hpp>

void init_feature_detector(cv::Ptr<cv::FeatureDetector> & fd,
		cv::Ptr<cv::DescriptorExtractor> & de,
		cv::Ptr<cv::DescriptorMatcher> & dm) {
	de = new cv::SurfDescriptorExtractor;
	dm = new cv::FlannBasedMatcher;
	fd = new cv::SurfFeatureDetector;

	fd->setInt("hessianThreshold", 400);
	fd->setInt("extended", 1);
	fd->setInt("upright", 1);
	fd->setInt("nOctaves", 8);
	fd->setInt("nOctaveLayers", 2);

	de->setInt("extended", 1);
	de->setInt("upright", 1);
	de->setInt("nOctaves", 8);
	de->setInt("nOctaveLayers", 2);

}

bool estimate_transform_ransac(const pcl::PointCloud<pcl::PointXYZ> & src,
		const pcl::PointCloud<pcl::PointXYZ> & dst,
		const std::vector<cv::DMatch> matches, int num_iter,
		float distance2_threshold, int min_num_inliers, Eigen::Affine3f & trans,
		std::vector<bool> & inliers) {

	int max_inliers = 0;

	if (matches.size() < min_num_inliers)
		return false;

	for (int iter = 0; iter < num_iter; iter++) {

		int rand_idx[3];
		// Select 3 random points
		for (int i = 0; i < 3; i++) {
			rand_idx[i] = rand() % matches.size();
		}

		while (rand_idx[0] == rand_idx[1] || rand_idx[0] == rand_idx[2]
				|| rand_idx[1] == rand_idx[2]) {
			for (int i = 0; i < 3; i++) {
				rand_idx[i] = rand() % matches.size();
			}
		}

		//std::cerr << "Random idx " << rand_idx[0] << " " << rand_idx[1] << " "
		//		<< rand_idx[2] << " " << matches.size() << std::endl;

		Eigen::Matrix3f src_rand, dst_rand;

		for (int i = 0; i < 3; i++) {
			src_rand.col(i) =
					src[matches[rand_idx[i]].queryIdx].getVector3fMap();
			dst_rand.col(i) =
					dst[matches[rand_idx[i]].trainIdx].getVector3fMap();

		}

		Eigen::Affine3f transformation;
		transformation = Eigen::umeyama(src_rand, dst_rand, false);

		//std::cerr << "src_rand " << std::endl << src_rand << std::endl;
		//std::cerr << "dst_rand " << std::endl << dst_rand << std::endl;
		//std::cerr << "src_rand_trans " << std::endl << transformation * src_rand
		//		<< std::endl;
		//std::cerr << "trans " << std::endl << transformation.matrix()
		//		<< std::endl;

		int current_num_inliers = 0;
		std::vector<bool> current_inliers;
		current_inliers.resize(matches.size());
		for (size_t i = 0; i < matches.size(); i++) {

			Eigen::Vector4f distance_vector = transformation
					* src[matches[i].queryIdx].getVector4fMap()
					- dst[matches[i].trainIdx].getVector4fMap();

			current_inliers[i] = distance_vector.squaredNorm()
					< distance2_threshold;
			if (current_inliers[i])
				current_num_inliers++;

		}

		if (current_num_inliers > max_inliers) {
			max_inliers = current_num_inliers;
			inliers = current_inliers;
		}
	}

	if (max_inliers < min_num_inliers) {
		return false;
	}

	Eigen::Matrix3Xf src_rand(3, max_inliers), dst_rand(3, max_inliers);

	int col_idx = 0;
	for (size_t i = 0; i < inliers.size(); i++) {
		if (inliers[i]) {
			src_rand.col(col_idx) = src[matches[i].queryIdx].getVector3fMap();
			dst_rand.col(col_idx) = dst[matches[i].trainIdx].getVector3fMap();
			col_idx++;
		}

	}

	trans = Eigen::umeyama(src_rand, dst_rand, false);
	trans.makeAffine();

	std::cerr << max_inliers << std::endl;

	return true;

}
void compute_features(const cv::Mat & rgb, const cv::Mat & depth,
		const Eigen::Vector3f & intrinsics, cv::Ptr<cv::FeatureDetector> & fd,
		cv::Ptr<cv::DescriptorExtractor> & de,
		std::vector<cv::KeyPoint> & filtered_keypoints,
		pcl::PointCloud<pcl::PointXYZ> & keypoints3d, cv::Mat & descriptors) {
	cv::Mat gray;

	if (rgb.channels() != 1) {
		cv::cvtColor(rgb, gray, cv::COLOR_BGR2GRAY);
	} else {
		gray = rgb;
	}

	cv::GaussianBlur(gray, gray, cv::Size(3, 3), 3);

	int threshold = 400;
	fd->setInt("hessianThreshold", threshold);

	//int threshold = 100;
	//fd->setInt("thres", threshold);

	std::vector<cv::KeyPoint> keypoints;

	cv::Mat mask(depth.size(), CV_8UC1);
	depth.convertTo(mask, CV_8U);

	fd->detect(gray, keypoints, mask);

	for (int i = 0; i < 5; i++) {
		if (keypoints.size() < 300) {
			threshold = threshold / 2;
			fd->setInt("hessianThreshold", threshold);
			//fd->setInt("thres", threshold);
			keypoints.clear();
			fd->detect(gray, keypoints, mask);
		} else {
			break;
		}
	}

	if (keypoints.size() > 400)
		keypoints.resize(400);

	filtered_keypoints.clear();
	keypoints3d.clear();

	for (size_t i = 0; i < keypoints.size(); i++) {
		if (depth.at<unsigned short>(keypoints[i].pt) != 0) {
			filtered_keypoints.push_back(keypoints[i]);

			pcl::PointXYZ p;
			p.z = depth.at<unsigned short>(keypoints[i].pt) / 1000.0f;
			p.x = (keypoints[i].pt.x - intrinsics[1]) * p.z / intrinsics[0];
			p.y = (keypoints[i].pt.y - intrinsics[2]) * p.z / intrinsics[0];

			//ROS_INFO("Point %f %f %f from  %f %f ", p.x, p.y, p.z, keypoints[i].pt.x, keypoints[i].pt.y);

			keypoints3d.push_back(p);

		}
	}

	de->compute(gray, filtered_keypoints, descriptors);
}

keyframe_map::keyframe_map() {
}

void keyframe_map::add_frame(const rm_localization::Keyframe::ConstPtr & k) {
	frames.push_back(color_keyframe::from_msg(k));
	idx.push_back(k->idx);

}

float keyframe_map::optimize_panorama(int level) {

	float iteration_max_update;
	int size = frames.size();

	tbb::concurrent_vector<std::pair<int, int> > overlaping_keyframes;

	for (int i = 0; i < size; i++) {
		for (int j = 0; j < size; j++) {
			if (i != j) {
				float angle =
						frames[i]->get_pos().unit_quaternion().angularDistance(
								frames[j]->get_pos().unit_quaternion());

				if (angle < M_PI / 6) {
					overlaping_keyframes.push_back(std::make_pair(i, j));
					//ROS_INFO("Images %d and %d intersect with angular distance %f", i, j, angle*180/M_PI);
				}
			}
		}
	}

	reduce_jacobian_rgb rj(frames, size, level);

	tbb::parallel_reduce(
			tbb::blocked_range<
					tbb::concurrent_vector<std::pair<int, int> >::iterator>(
					overlaping_keyframes.begin(), overlaping_keyframes.end()),
			rj);

	/*
	 rj(
	 tbb::blocked_range<
	 tbb::concurrent_vector<std::pair<int, int> >::iterator>(
	 overlaping_keyframes.begin(), overlaping_keyframes.end()));
	 */

	Eigen::VectorXf update = -rj.JtJ.ldlt().solve(rj.Jte);
	//Eigen::VectorXf update =
	//			-rj.JtJ.block(0, 0, size * 3, size* 3).ldlt().solve(
	//					rj.Jte.segment(0, size * 3));

	iteration_max_update = std::max(std::abs(update.maxCoeff()),
			std::abs(update.minCoeff()));

	ROS_INFO("Max update %f", iteration_max_update);

	for (int i = 0; i < size; i++) {

		frames[i]->get_pos().so3() = Sophus::SO3f::exp(update.segment<3>(i * 3))
				* frames[i]->get_pos().so3();

		frames[i]->get_pos().translation() = frames[0]->get_pos().translation();

		frames[i]->get_intrinsics().array() =
				update.segment<3>(size * 3).array().exp()
						* frames[i]->get_intrinsics().array();

		if (i == 0) {
			Eigen::Vector3f intrinsics = frames[i]->get_intrinsics();
			ROS_INFO("New intrinsics %f, %f, %f", intrinsics(0), intrinsics(1),
					intrinsics(2));
		}

	}

	return iteration_max_update;

}

bool keyframe_map::find_transform(const keyframe_map & other,
		Sophus::SE3f & t) const {

	if(frames.size() < 2 || other.frames.size() < 2) {
		return false;
	}

	int i = rand() % frames.size();
	int j = rand() % other.frames.size();

	cv::imshow("i", frames[i]->get_rgb());
	cv::imshow("j", other.frames[j]->get_rgb());
	cv::waitKey(3);

	cv::Ptr<cv::FeatureDetector> fd;
	cv::Ptr<cv::DescriptorExtractor> de;
	cv::Ptr<cv::DescriptorMatcher> dm;

	init_feature_detector(fd, de, dm);

	std::vector<cv::KeyPoint> keypoints_i, keypoints_j;
	pcl::PointCloud<pcl::PointXYZ> keypoints3d_i, keypoints3d_j;
	cv::Mat descriptors_i, descriptors_j;

	compute_features(frames[i]->get_rgb(), frames[i]->get_d(0),
			frames[i]->get_intrinsics(0), fd, de, keypoints_i, keypoints3d_i,
			descriptors_i);

	compute_features(other.frames[j]->get_rgb(), other.frames[j]->get_d(0),
			other.frames[j]->get_intrinsics(0), fd, de, keypoints_j,
			keypoints3d_j, descriptors_j);

	std::vector<cv::DMatch> matches, matches_filtered;
	dm->match(descriptors_j, descriptors_i, matches);

	Eigen::Affine3f transform;
	std::vector<bool> inliers;

	bool res = estimate_transform_ransac(keypoints3d_j, keypoints3d_i, matches,
			5000, 0.03 * 0.03, 20, transform, inliers);

	if (res) {

		Sophus::SE3f t = frames[i]->get_pos()
				* Sophus::SE3f(transform.rotation(), transform.translation())
				* other.frames[j]->get_pos().inverse();

		for (size_t k = 0; k < matches.size(); k++) {
			if (inliers[k]) {
				matches_filtered.push_back(matches[k]);
			}
		}

		cv::Mat matches_img;
		cv::drawMatches(other.frames[j]->get_rgb(), keypoints_j,
				frames[i]->get_rgb(), keypoints_i, matches_filtered,
				matches_img, cv::Scalar(0, 255, 0));
		cv::imshow("Matches", matches_img);
		cv::waitKey(0);

		return true;

	}

	return false;

}

void keyframe_map::merge(keyframe_map & other, const Sophus::SE3f & t) {

	for (size_t iter = 0; iter < other.frames.size(); iter++) {
		other.frames[iter]->get_pos() = t * other.frames[iter]->get_pos();
		frames.push_back(other.frames[iter]);
	}

	other.frames.clear();

}

float keyframe_map::optimize(int level) {

	float iteration_max_update;
	int size = frames.size();

	tbb::concurrent_vector<std::pair<int, int> > overlaping_keyframes;

	for (int i = 0; i < size; i++) {
		for (int j = 0; j < size; j++) {
			if (i != j) {
				float angle =
						frames[i]->get_pos().unit_quaternion().angularDistance(
								frames[j]->get_pos().unit_quaternion());

				//float centroid_distance = (frames[i]->get_centroid()
				//		- frames[j]->get_centroid()).norm();

				float distance = (frames[i]->get_pos().translation()
						- frames[j]->get_pos().translation()).norm();

				if (angle < M_PI / 6 && distance < 0.5) {
					overlaping_keyframes.push_back(std::make_pair(i, j));
					//ROS_INFO("Images %d and %d intersect with angular distance %f", i, j, angle*180/M_PI);
				}
			}
		}
	}

	reduce_jacobian_rgb_3d rj(frames, size, level);
	/*
	 tbb::parallel_reduce(
	 tbb::blocked_range<
	 tbb::concurrent_vector<std::pair<int, int> >::iterator>(
	 overlaping_keyframes.begin(), overlaping_keyframes.end()),
	 rj);

	 */
	rj(
			tbb::blocked_range<
					tbb::concurrent_vector<std::pair<int, int> >::iterator>(
					overlaping_keyframes.begin(), overlaping_keyframes.end()));

	Eigen::VectorXf update =
			-rj.JtJ.block(6, 6, (size - 1) * 6, (size - 1) * 6).ldlt().solve(
					rj.Jte.segment(6, (size - 1) * 6));

	iteration_max_update = std::max(std::abs(update.maxCoeff()),
			std::abs(update.minCoeff()));

	ROS_INFO("Max update %f", iteration_max_update);

	for (int i = 0; i < size - 1; i++) {
		frames[i + 1]->get_pos() = Sophus::SE3f::exp(update.segment<6>(i * 6))
				* frames[i + 1]->get_pos();
	}

	return iteration_max_update;

}

cv::Mat keyframe_map::get_panorama_image() {
	cv::Mat res = cv::Mat::zeros(512, 1024, CV_32F);
	cv::Mat w = cv::Mat::zeros(res.size(), res.type());

	cv::Mat map_x(res.size(), res.type()), map_y(res.size(), res.type()),
			img_projected(res.size(), res.type()), mask(res.size(), res.type());

	cv::Mat image_weight(frames[0]->get_rgb().size(), res.type()),
			intencity_weighted(image_weight.size(), image_weight.type());

	float cx = image_weight.cols / 2.0;
	float cy = image_weight.rows / 2.0;
	float max_r = cy * cy;
	for (int v = 0; v < image_weight.rows; v++) {
		for (int u = 0; u < image_weight.cols; u++) {
			image_weight.at<float>(v, u) = std::max(
					1.0 - ((u - cx) * (u - cx) + (v - cy) * (v - cy)) / max_r,
					0.0);
		}
	}

//cv::imshow("image_weight", image_weight);
//cv::waitKey(0);

	cx = res.cols / 2.0;
	cy = res.rows / 2.0;

	float scale_x = 2 * M_PI / res.cols;
	float scale_y = M_PI / res.rows;

	for (size_t i = 0; i < frames.size(); i++) {

		Eigen::Vector3f intrinsics = frames[i]->get_intrinsics(0);
		Eigen::Quaternionf Qiw =
				frames[i]->get_pos().unit_quaternion().inverse();

		Eigen::Matrix3f K;
		K << intrinsics[0], 0, intrinsics[1], 0, intrinsics[0], intrinsics[2], 0, 0, 1;
		Eigen::Matrix3f H = K * Qiw.matrix();

		for (int v = 0; v < map_x.rows; v++) {
			for (int u = 0; u < map_x.cols; u++) {

				float phi = (u - cx) * scale_x;
				float theta = (v - cy) * scale_y;

				Eigen::Vector3f vec(cos(theta) * cos(phi),
						-cos(theta) * sin(phi), -sin(theta));
				//Eigen::Vector3f vec(cos(theta) * sin(phi), sin(theta),
				//		cos(theta) * cos(phi));

				vec = H * vec;

				if (vec[2] > 0.01) {
					map_x.at<float>(v, u) = vec[0] / vec[2];
					map_y.at<float>(v, u) = vec[1] / vec[2];
				} else {
					map_x.at<float>(v, u) = -1;
					map_y.at<float>(v, u) = -1;
				}
			}
		}

		img_projected = 0.0f;
		mask = 0.0f;
		cv::Mat intencity;
		frames[i]->get_i(0).convertTo(intencity, CV_32F, 1.0 / 255);
		cv::multiply(intencity, image_weight, intencity_weighted);
		cv::remap(intencity_weighted, img_projected, map_x, map_y,
				CV_INTER_LINEAR, cv::BORDER_TRANSPARENT, 0);
		cv::remap(image_weight, mask, map_x, map_y, CV_INTER_LINEAR,
				cv::BORDER_TRANSPARENT, 0);

		res += img_projected;
		w += mask;

		//cv::imshow("intencity", intencity);
		//cv::imshow("intencity_weighted", intencity_weighted);
		//cv::imshow("img_projected", img_projected);
		//cv::imshow("mask", mask);
		//cv::waitKey(0);

	}

	return res / w;
}

void keyframe_map::save(const std::string & dir_name) {

	if (boost::filesystem::exists(dir_name)) {
		boost::filesystem::remove_all(dir_name);
	}

	boost::filesystem::create_directory(dir_name);
	boost::filesystem::create_directory(dir_name + "/rgb");
	boost::filesystem::create_directory(dir_name + "/depth");

	for (size_t i = 0; i < frames.size(); i++) {
		cv::imwrite(
				dir_name + "/rgb/" + boost::lexical_cast<std::string>(i)
						+ ".png", frames[i]->get_rgb());
		cv::imwrite(
				dir_name + "/depth/" + boost::lexical_cast<std::string>(i)
						+ ".png", frames[i]->get_d(0));

	}

	std::ofstream f((dir_name + "/positions.txt").c_str(),
			std::ios_base::binary);
	for (size_t i = 0; i < frames.size(); i++) {
		Eigen::Quaternionf q = frames[i]->get_pos().unit_quaternion();
		Eigen::Vector3f t = frames[i]->get_pos().translation();
		Eigen::Vector3f intrinsics = frames[i]->get_intrinsics(0);

		f.write((char *) q.coeffs().data(), sizeof(float) * 4);
		f.write((char *) t.data(), sizeof(float) * 3);
		f.write((char *) intrinsics.data(), sizeof(float) * 3);

	}
	f.close();

}

void keyframe_map::load(const std::string & dir_name) {

	std::vector<std::pair<Sophus::SE3f, Eigen::Vector3f> > positions;

	std::ifstream f((dir_name + "/positions.txt").c_str(),
			std::ios_base::binary);
	while (f) {
		Eigen::Quaternionf q;
		Eigen::Vector3f t;
		Eigen::Vector3f intrinsics;

		f.read((char *) q.coeffs().data(), sizeof(float) * 4);
		f.read((char *) t.data(), sizeof(float) * 3);
		f.read((char *) intrinsics.data(), sizeof(float) * 3);

		positions.push_back(std::make_pair(Sophus::SE3f(q, t), intrinsics));
	}

	positions.pop_back();
	std::cerr << "Loaded " << positions.size() << " positions" << std::endl;

	for (size_t i = 0; i < positions.size(); i++) {
		cv::Mat rgb = cv::imread(
				dir_name + "/rgb/" + boost::lexical_cast<std::string>(i)
						+ ".png", CV_LOAD_IMAGE_UNCHANGED);
		cv::Mat depth = cv::imread(
				dir_name + "/depth/" + boost::lexical_cast<std::string>(i)
						+ ".png", CV_LOAD_IMAGE_UNCHANGED);

		cv::Mat gray;
		cv::cvtColor(rgb, gray, CV_RGB2GRAY);

		color_keyframe::Ptr k(
				new color_keyframe(rgb, gray, depth, positions[i].first,
						positions[i].second));
		frames.push_back(k);
	}

}

pcl::PointCloud<pcl::PointXYZRGB>::Ptr keyframe_map::get_map_pointcloud() {
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr res(
			new pcl::PointCloud<pcl::PointXYZRGB>);

	for (size_t i = 0; i < frames.size(); i++) {

		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud =
				frames[i]->get_colored_pointcloud(4);

		*res += *cloud;

	}

	return res;

}
