// Copyright (c) 2016 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vpx_image_transport/vpx_publisher.h"

#include <cv_bridge/cv_bridge.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <vpx/vp8cx.h>

namespace vpx_image_transport {

  using namespace webm_tools;
  using namespace mkvmuxer;

VPXPublisher::VPXPublisher()
  : codec_context_(NULL), encoder_config_(NULL),
    frame_count_(0), keyframe_forced_interval_(4), muxer_(NULL) {

}

VPXPublisher::~VPXPublisher() {
  muxer_->Finalize();
  vpx_codec_destroy(codec_context_);
  delete muxer_;
  delete codec_context_;
  delete encoder_config_;
  muxer_ = NULL;
  codec_context_ = NULL;
  encoder_config_ = NULL;
}

void VPXPublisher::advertiseImpl(ros::NodeHandle &nh,
    const std::string& base_topic,
    uint32_t queue_size,
    const image_transport::SubscriberStatusCallback& user_connect_cb,
    const image_transport::SubscriberStatusCallback& user_disconnect_cb,
    const ros::VoidPtr& tracked_object,
    bool latch) {
  typedef image_transport::SimplePublisherPlugin<vpx_image_transport::Packet> Base;
  Base::advertiseImpl(nh, base_topic, queue_size, user_connect_cb,
    user_disconnect_cb, tracked_object, latch);

  // Set up reconfigure server for this topic
  reconfigure_server_ = boost::make_shared<ReconfigureServer>(this->nh());
  ReconfigureServer::CallbackType f =
      boost::bind(&VPXPublisher::configCallback, this, _1, _2);
  reconfigure_server_->setCallback(f);
}

void VPXPublisher::configCallback(Config& config, uint32_t level) {
  vpx_codec_err_t ret;
  if (!encoder_config_) {
    encoder_config_ = new vpx_codec_enc_cfg();
    ret = vpx_codec_enc_config_default(vpx_codec_vp8_cx(), encoder_config_, 0);
    if (ret) {
      ROS_ERROR("Failed to get default encoder configuration. Error No.: %d", ret);
    }
  }

  encoder_config_->g_w = config.width;
  encoder_config_->g_h = config.height;
  encoder_config_->g_threads = config.threads;
  encoder_config_->rc_resize_allowed = config.resize_allowed;
  encoder_config_->rc_scaled_width = config.scaled_width;
  encoder_config_->rc_scaled_height = config.scaled_height;
  encoder_config_->rc_end_usage = static_cast<vpx_rc_mode>(config.end_usage);
  encoder_config_->rc_target_bitrate = config.target_bitrate;

  // Keyframe configurations
  keyframe_forced_interval_ = config.keyframe_forced_interval;
  encoder_config_->kf_mode = static_cast<vpx_kf_mode>(config.keyframe_mode);
  encoder_config_->kf_min_dist = config.keyframe_min_interval;
  encoder_config_->kf_max_dist = config.keyframe_max_interval;

  if (!codec_context_) {
    codec_context_ = new vpx_codec_ctx_t();
    ret = vpx_codec_enc_init(codec_context_, vpx_codec_vp8_cx(), encoder_config_, 0);
    if (ret) {
      ROS_ERROR("Failed to initialize VPX encoder. Error No.:%d", ret);
    }
  } else {
    ret = vpx_codec_enc_config_set(codec_context_, encoder_config_);
    if (ret) {
      ROS_ERROR("Failed to update codec configuration. Error No.:%d", ret);
    }
  }

  frame_count_ = 0;
}

int vpx_img_plane_width(const vpx_image_t *img, int plane) {
  if (plane > 0 && img->x_chroma_shift > 0)
    return (img->d_w + 1) >> img->x_chroma_shift;
  else
    return img->d_w;
}

int vpx_img_plane_height(const vpx_image_t *img, int plane) {
  if (plane > 0 &&  img->y_chroma_shift > 0)
    return (img->d_h + 1) >> img->y_chroma_shift;
  else
    return img->d_h;
}

void VPXPublisher::publish(const sensor_msgs::Image& message,
                           const PublishFn& publish_fn) const {
  // conversion necessary
  if (!sensor_msgs::image_encodings::isColor(message.encoding)
      && !sensor_msgs::image_encodings::isMono(message.encoding)) {
    ROS_ERROR("VPX publisher is not able to handle encoding type:%s", message.encoding.c_str());
    return;
  }

  cv_bridge::CvImageConstPtr cv_image_ptr;
  try {
    cv_image_ptr = cv_bridge::toCvCopy(message, sensor_msgs::image_encodings::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: '%s'", e.what());
    return;
  } catch (cv::Exception& e) {
    ROS_ERROR("OpenCV exception: '%s'", e.what());
    return;
  }
  if (cv_image_ptr == 0) {
    ROS_ERROR("Unable to convert from '%s' to 'bgr8'", message.encoding.c_str());
    return;
  }

  const cv::Mat bgr = cv_image_ptr->image;

  cv::Mat bgr_padded;
  const int frame_width = message.width, frame_height = message.height;

  if (frame_width == bgr.cols && frame_height == bgr.rows) {
    bgr_padded = bgr;
  } else {
    bgr_padded = cv::Mat::zeros(frame_height, frame_width, bgr.type());
    cv::Mat pic_roi = bgr_padded(cv::Rect(0, 0, bgr.cols, bgr.rows));
    bgr.copyTo(pic_roi);
  }

  // Convert image to i420 color space used by vpx
  cv::Mat i420;
  cv::cvtColor(bgr_padded, i420, cv::COLOR_BGR2YUV_I420);

  vpx_image_t image;
  if (!vpx_img_alloc(&image, VPX_IMG_FMT_I420, frame_width, frame_height, 1)) {
    ROS_ERROR("Failed to allocate vpx image.");
    return;
  }
  unsigned char* org = i420.data;
  for (int plane = 0; plane < 3; ++plane) {
    unsigned char* buf = image.planes[plane];
    const int w = vpx_img_plane_width(&image, plane);
    const int h = vpx_img_plane_height(&image, plane);
    for (int y = 0; y < h; ++y) {
      memcpy(buf, org, w);
      buf += w;
      org += w;
    }
  }

  int flags = 0;
  if (frame_count_ % keyframe_forced_interval_ == 0)
    flags |= VPX_EFLAG_FORCE_KF;

  vpx_codec_iter_t iter = NULL;
  const vpx_codec_cx_pkt_t *pkt = NULL;
  const vpx_codec_err_t ret = vpx_codec_encode(codec_context_, &image,
     frame_count_++, 1, flags, VPX_DL_REALTIME);
  if (ret != VPX_CODEC_OK) {
    vpx_img_free(&image);
    return;
  }

  while ((pkt = vpx_codec_get_cx_data(codec_context_, &iter)) != NULL) {
    if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
      bool keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
      muxer_->WriteVideoFrame(reinterpret_cast<uint8*>(pkt->data.frame.buf), pkt->data.frame.sz, frame_count_, keyframe);
    } else {
      ROS_INFO("pkt->kind: %d", pkt->kind);
    }
  }
  sendChunkIfReady(publish_fn);
  vpx_img_free(&image);
}

void VPXPublisher::sendChunkIfReady(const PublishFn &publish_fn) const {
  webm_tools::int32 chunk_length = 0;
  if (!muxer_->ChunkReady(&chunk_length)) {
    return;
  }

  vpx_image_transport::Packet packet;
  packet.data.resize(chunk_length);
  int ret = muxer_->ReadChunk(chunk_length, &packet.data[0]);
  if (WebMLiveMuxer::kSuccess != ret) {
    ROS_ERROR("Failed to read chunk with error code: %d", ret);
    return;
  }
  packet.header.seq = frame_count_;
  packet.header.stamp = ros::Time::now();
  publish_fn(packet);
}

void VPXPublisher::connectCallback(const ros::SingleSubscriberPublisher& pub) {
  if (muxer_) {
    muxer_->Finalize();
    delete muxer_;
  }
  muxer_ = new webm_tools::WebMLiveMuxer();
  muxer_->Init();
  muxer_->AddVideoTrack(encoder_config_->g_w, encoder_config_->g_h);
}

void VPXPublisher::disconnectCallback(const ros::SingleSubscriberPublisher &pub) {
  int ret = muxer_->Finalize();
  if (ret != WebMLiveMuxer::kSuccess) {
    ROS_ERROR("Failed to finalize live muxer with error code: %d", ret);
    return;
  }
  int chunk_length = 0;
  if (!muxer_->ChunkReady(&chunk_length)) {
    ROS_ERROR("Failed to get chunk after finalized called.");
  }
  delete muxer_;
  muxer_ = 0;
}

} // vpx_image_transport