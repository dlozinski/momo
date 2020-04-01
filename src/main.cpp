#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "rtc_base/log_sinks.h"

#if USE_ROS
#include "ros/ros_log_sink.h"
#include "ros/ros_video_capture.h"
#include "signal_listener.h"
#else
#if defined(__APPLE__)
#include "mac_helper/mac_capturer.h"
#elif defined(__linux__)
#if USE_MMAL_ENCODER
#include "hwenc_mmal/mmal_v4l2_capture.h"
#endif
#include "v4l2_video_capturer/v4l2_video_capturer.h"
#else
#include "rtc/device_video_capturer.h"
#endif
#endif

#include "serial_data_channel/serial_data_manager.h"

#if USE_SDL2
#include "sdl_renderer/sdl_renderer.h"
#endif

#include "ayame/ayame_server.h"
#include "connection_settings.h"
#include "p2p/p2p_server.h"
#include "rtc/manager.h"
#include "sora/sora_server.h"
#include "util.h"

const size_t kDefaultMaxLogFileSize = 10 * 1024 * 1024;

int main(int argc, char* argv[]) {
  ConnectionSettings cs;

  bool use_test = false;
  bool use_ayame = false;
  bool use_sora = false;
  int log_level = rtc::LS_NONE;

  Util::parseArgs(argc, argv, use_test, use_ayame, use_sora, log_level, cs);

  rtc::LogMessage::LogToDebug((rtc::LoggingSeverity)log_level);
  rtc::LogMessage::LogTimestamps();
  rtc::LogMessage::LogThreads();

#if USE_ROS
  std::unique_ptr<rtc::LogSink> log_sink(new ROSLogSink());
  rtc::LogMessage::AddLogToStream(log_sink.get(), rtc::LS_INFO);
#else
  std::unique_ptr<rtc::FileRotatingLogSink> log_sink(
      new rtc::FileRotatingLogSink("./", "webrtc_logs", kDefaultMaxLogFileSize,
                                   10));
  if (!log_sink->Init()) {
    RTC_LOG(LS_ERROR) << __FUNCTION__ << "Failed to open log file";
    log_sink.reset();
    return 1;
  }
  rtc::LogMessage::AddLogToStream(log_sink.get(), rtc::LS_INFO);
#endif

  auto capturer = ([&]() -> rtc::scoped_refptr<ScalableVideoTrackSource> {
    if (cs.no_video_device) {
      return nullptr;
    }

#if USE_ROS
    rtc::scoped_refptr<ROSVideoCapture> capturer(
        new rtc::RefCountedObject<ROSVideoCapture>(cs));
    return capturer;
#else  // USE_ROS
    auto size = cs.getSize();
#if defined(__APPLE__)
    return MacCapturer::Create(size.width, size.height, cs.framerate,
                               cs.video_device);
#elif defined(__linux__)
#if USE_MMAL_ENCODER
    if (cs.use_native) {
      return MMALV4L2Capture::Create(cs);
    } else {
      return V4L2VideoCapture::Create(cs);
    }
#else
    return V4L2VideoCapture::Create(cs);
#endif
#else
    return DeviceVideoCapturer::Create(size.width, size.height, cs.framerate,
                                       cs.video_device);
#endif
#endif  // USE_ROS
  })();

  if (!capturer && !cs.no_video_device) {
    std::cerr << "failed to create capturer" << std::endl;
    return 1;
  }

#if USE_SDL2
  std::unique_ptr<SDLRenderer> sdl_renderer = nullptr;
  if (cs.use_sdl) {
    sdl_renderer.reset(
        new SDLRenderer(cs.window_width, cs.window_height, cs.fullscreen));
  }

  std::unique_ptr<RTCManager> rtc_manager(
      new RTCManager(cs, std::move(capturer), sdl_renderer.get()));
#else
  std::unique_ptr<RTCManager> rtc_manager(
      new RTCManager(cs, std::move(capturer), nullptr));
#endif

  {
    boost::asio::io_context ioc{1};

    std::unique_ptr<RTCDataManager> data_manager = nullptr;
    if (!cs.serial_device.empty()) {
      data_manager =
          SerialDataManager::Create(ioc, cs.serial_device, cs.serial_rate);
      if (!data_manager) {
        return 1;
      }
      rtc_manager->SetDataManager(data_manager.get());
    }

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](const boost::system::error_code&, int) { ioc.stop(); });

    if (use_sora) {
      if (cs.sora_port >= 0) {
        const boost::asio::ip::tcp::endpoint endpoint{
            boost::asio::ip::make_address("127.0.0.1"),
            static_cast<unsigned short>(cs.sora_port)};
        std::make_shared<SoraServer>(ioc, endpoint, rtc_manager.get(), cs)
            ->run();
      } else {
        std::make_shared<SoraServer>(ioc, rtc_manager.get(), cs)->run();
      }
    }

    if (use_test) {
      const boost::asio::ip::tcp::endpoint endpoint{
          boost::asio::ip::make_address("0.0.0.0"),
          static_cast<unsigned short>(cs.test_port)};
      std::make_shared<P2PServer>(
          ioc, endpoint, std::make_shared<std::string>(cs.test_document_root),
          rtc_manager.get(), cs)
          ->run();
    }

    if (use_ayame) {
      std::make_shared<AyameServer>(ioc, rtc_manager.get(), cs)->run();
    }

#if USE_SDL2
    if (sdl_renderer) {
      sdl_renderer->SetDispatchFunction([&ioc](std::function<void()> f) {
        if (ioc.stopped())
          return;
        boost::asio::dispatch(ioc.get_executor(), f);
      });

      ioc.run();

      sdl_renderer->SetDispatchFunction(nullptr);
    } else {
      ioc.run();
    }
#else
    ioc.run();
#endif
  }

  //この順番は綺麗に落ちるけど、あまり安全ではない
#if USE_SDL2
  sdl_renderer = nullptr;
#endif
  rtc_manager = nullptr;

  return 0;
}
