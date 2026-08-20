#include "archive.h"

#include <CL/opencl.h>

#include "R3DSDK.h"
#include "R3DSDKOpenCL.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace nraw {

namespace {

constexpr size_t kInFlight = 8;

bool bindOpenCL(void* h, R3DSDK::EXT_OCLAPI_1_1& ocl)
{
    if (!h)
        return false;
    bool ok = true;
#define BIND(f) \
    ocl.f = reinterpret_cast<decltype(ocl.f)>(dlsym(h, #f)); \
    ok = ok && ocl.f != nullptr;
    BIND(clSetKernelArg);
    BIND(clFlush);
    BIND(clFinish);
    BIND(clEnqueueCopyImage);
    BIND(clCreateContext);
    BIND(clCreateCommandQueue);
    BIND(clCreateSampler);
    BIND(clCreateKernel);
    BIND(clCreateBuffer);
    BIND(clCreateProgramWithSource);
    BIND(clCreateProgramWithBinary);
    BIND(clReleaseEvent);
    BIND(clReleaseSampler);
    BIND(clReleaseKernel);
    BIND(clReleaseMemObject);
    BIND(clReleaseProgram);
    BIND(clReleaseContext);
    BIND(clReleaseCommandQueue);
    BIND(clGetPlatformInfo);
    BIND(clGetDeviceIDs);
    BIND(clGetPlatformIDs);
    BIND(clGetDeviceInfo);
    BIND(clGetContextInfo);
    BIND(clGetImageInfo);
    BIND(clGetProgramBuildInfo);
    BIND(clGetProgramInfo);
    BIND(clGetKernelWorkGroupInfo);
    BIND(clBuildProgram);
    BIND(clEnqueueWriteBuffer);
    BIND(clEnqueueReadBuffer);
    BIND(clEnqueueCopyBuffer);
    BIND(clEnqueueCopyBufferToImage);
    BIND(clEnqueueWriteImage);
    BIND(clEnqueueNDRangeKernel);
    BIND(clEnqueueMapBuffer);
    BIND(clEnqueueUnmapMemObject);
    BIND(clWaitForEvents);
    BIND(clEnqueueBarrier);
    BIND(clEnqueueMarker);
    BIND(clCreateImage2D);
    BIND(clSetMemObjectDestructorCallback);
    BIND(clCreateSubBuffer);
    BIND(clGetMemObjectInfo);
    BIND(clCreateImage3D);
#undef BIND
    return ok;
}

bool mkdirChain(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && !cur.empty()) {
            if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
        cur += path[i];
    }
    if (!cur.empty() && mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    return true;
}

std::string openclCacheDir(std::string& err)
{
    std::string base;
    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    if (xdg && *xdg)
        base = xdg;
    else if (home && *home)
        base = std::string(home) + "/.cache";
    else
        return "";
    std::string dir = base + "/nraw-archive/opencl";
    if (!mkdirChain(dir)) {
        err = "无法创建 OpenCL 内核缓存目录 " + dir;
        return "";
    }
    return dir;
}

class ClEngine {
public:
    ~ClEngine() { destroy(); }

    bool init(R3DSDK::EXT_OCLAPI_1_1& ocl, std::string& deviceName, std::string& err)
    {
        ocl_ = &ocl;
        cl_uint n = 0;
        if (ocl_->clGetPlatformIDs(0, nullptr, &n) != CL_SUCCESS || n == 0) {
            err = "未找到 OpenCL 平台（无 OpenCL 驱动）";
            return false;
        }
        std::vector<cl_platform_id> plats(n);
        if (ocl_->clGetPlatformIDs(n, plats.data(), nullptr) != CL_SUCCESS) {
            err = "clGetPlatformIDs 失败";
            return false;
        }
        auto vendorOf = [&](cl_device_id d) -> std::string {
            char buf[256] = {0};
            size_t sz = 0;
            if (ocl_->clGetDeviceInfo(d, CL_DEVICE_VENDOR, sizeof(buf) - 1, buf, &sz) !=
                CL_SUCCESS)
                return std::string();
            return std::string(buf);
        };
        auto nameOf = [&](cl_device_id d) -> std::string {
            char buf[256] = {0};
            size_t sz = 0;
            if (ocl_->clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof(buf) - 1, buf, &sz) !=
                CL_SUCCESS)
                return std::string();
            return std::string(buf);
        };
        auto isNvidia = [](const std::string& v) {
            std::string l = v;
            for (size_t i = 0; i < l.size(); ++i)
                if (l[i] >= 'A' && l[i] <= 'Z')
                    l[i] = static_cast<char>(l[i] - 'A' + 'a');
            return l.find("nvidia") != std::string::npos;
        };
        // SDK 仅支持 OpenCL 1.1 及以上（官方样例同样跳过 1.0 平台）
        auto platformIsOpenCL10 = [&](cl_platform_id p) -> bool {
            char buf[256] = {0};
            if (ocl_->clGetPlatformInfo(p, CL_PLATFORM_VERSION, sizeof(buf) - 1, buf,
                                        nullptr) != CL_SUCCESS)
                return false;
            return strstr(buf, "OpenCL 1.0") != nullptr;
        };
        // 老 Intel HD 核显驱动对 REDCL 支持差，跳过（Intel Iris 可用，允许）
        auto isOldIntel = [](const std::string& vendor, const std::string& name) -> bool {
            if (name.find("Iris") != std::string::npos)
                return false;
            if (vendor.find("Intel") != std::string::npos)
                return true;
            if (name.find("HD Graphics") != std::string::npos)
                return true;
            if (name.find("Intel") != std::string::npos)
                return true;
            return false;
        };
        for (cl_platform_id p : plats) {
            if (platformIsOpenCL10(p))
                continue;
            cl_uint nd = 0;
            if (ocl_->clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, 0, nullptr, &nd) != CL_SUCCESS ||
                nd == 0)
                continue;
            std::vector<cl_device_id> devs(nd);
            if (ocl_->clGetDeviceIDs(p, CL_DEVICE_TYPE_GPU, nd, devs.data(), nullptr) !=
                CL_SUCCESS)
                continue;
            for (cl_device_id d : devs) {
                cl_bool avail = CL_FALSE;
                if (ocl_->clGetDeviceInfo(d, CL_DEVICE_AVAILABLE, sizeof(avail), &avail,
                                          nullptr) != CL_SUCCESS || !avail)
                    continue;
                std::string v = vendorOf(d);
                if (isOldIntel(v, nameOf(d)))
                    continue;
                if (isNvidia(v)) {
                    platform_ = p;
                    device_ = d;
                    return finishInit(deviceName, err);
                }
                if (!device_) {
                    platform_ = p;
                    device_ = d;
                }
            }
        }
        if (!device_) {
            err = "未找到可用的 OpenCL GPU 设备";
            return false;
        }
        return finishInit(deviceName, err);
    }

    bool finishInit(std::string& deviceName, std::string& err)
    {
        char buf[512] = {0};
        size_t sz = 0;
        if (ocl_->clGetDeviceInfo(device_, CL_DEVICE_NAME, sizeof(buf) - 1, buf, &sz) ==
            CL_SUCCESS)
            deviceName = buf;
        cl_int ce = 0;
        cl_context_properties props[] = {
            CL_CONTEXT_PLATFORM, reinterpret_cast<cl_context_properties>(platform_), 0};
        context_ = ocl_->clCreateContext(props, 1, &device_, nullptr, nullptr, &ce);
        if (!context_ || ce != CL_SUCCESS) {
            err = "OpenCL 上下文创建失败 (clerr " + std::to_string(ce) + ")";
            destroy();
            return false;
        }
        queue_ = ocl_->clCreateCommandQueue(context_, device_, 0, &ce);
        if (!queue_ || ce != CL_SUCCESS) {
            err = "OpenCL 命令队列创建失败 (clerr " + std::to_string(ce) + ")";
            destroy();
            return false;
        }
        return true;
    }

    void destroy()
    {
        if (ocl_) {
            if (queue_) {
                ocl_->clReleaseCommandQueue(queue_);
                queue_ = nullptr;
            }
            if (context_) {
                ocl_->clReleaseContext(context_);
                context_ = nullptr;
            }
        }
        device_ = nullptr;
        platform_ = nullptr;
        ocl_ = nullptr;
    }

    R3DSDK::EXT_OCLAPI_1_1* ocl() const { return ocl_; }
    cl_context context() const { return context_; }
    cl_command_queue queue() const { return queue_; }

private:
    R3DSDK::EXT_OCLAPI_1_1* ocl_ = nullptr;
    cl_platform_id platform_ = nullptr;
    cl_device_id device_ = nullptr;
    cl_context context_ = nullptr;
    cl_command_queue queue_ = nullptr;
};

}

class GpuPipelineImpl {
public:
    ~GpuPipelineImpl() { close(); }

    bool init(const CliOptions& opt, const MediaInfo& info, std::string& err);
    // 无素材 GPU 测试：仅 OpenCL 加载/设备枚举 + REDCL 构造 + 内核编译，
    // 不打开剪辑、不分配缓冲（--gpu-test 无输入文件模式）
    bool initGpuOnly(std::string& err);
    bool decodeSync(size_t frameNo, VideoFrame& out, std::string& err);
    bool start(FrameQueue& frames, std::atomic<bool>* abort, size_t startFrame,
               std::string& err);
    bool submit(size_t frameNo, std::string& err);
    bool finish(std::string& err);
    void close();
    const std::string& deviceName() const { return deviceName_; }
    std::string lastError() const
    {
        std::lock_guard<std::mutex> lk(readyM_);
        return failMsg_;
    }

    void onReady(R3DSDK::AsyncDecompressJob* item, R3DSDK::DecodeStatus st);

private:
    struct Slot;
    struct CbData {
        GpuPipelineImpl* pipe = nullptr;
        Slot* slot = nullptr;
        size_t frameNo = 0;
    };
    struct Slot {
        AlignedBuffer raw;
        size_t rawSize = 0;
        R3DSDK::AsyncDecompressJob job;
        CbData cb;
        bool abandoned = false;
        // 多帧在途（processAsync 流水线）要求每帧独立 GPU 缓冲：
        cl_mem rawDev = nullptr;   // GPU 端 raw 缓冲（init 时创建）
        cl_mem outDev = nullptr;   // GPU 端输出缓冲（首次 debayer 时按需创建）
        AlignedBuffer outHost;     // 回读缓冲
        size_t outSize = 0;
    };
    struct ReadyJob {
        Slot* slot = nullptr;
        size_t frameNo = 0;
        R3DSDK::DecodeStatus status = R3DSDK::DSDecodeOK;
    };
    // 已提交 processAsync、等待 completeAsync 的帧（worker 线程独占，无需锁）
    struct Inflight {
        Slot* slot = nullptr;
        R3DSDK::DebayerOpenCLJob* job = nullptr;
        size_t frameNo = 0;
    };

    static void asyncCb(R3DSDK::AsyncDecompressJob* item, R3DSDK::DecodeStatus st);

    Slot* acquire();
    void release(Slot* s);
    // 异步 debayer 流水线两段：
    //   debayerStart:   上传 raw + processAsync（不等待 GPU），帧进入 inflight_
    //   debayerComplete: completeAsync（等该帧 GPU 完成）+ 回读 + planar 转换
    bool debayerStart(Slot* s, std::string& err);
    bool debayerComplete(Slot* s, VideoFrame& out, std::string& err);
    void worker();
    void setFail(const std::string& msg);

    // OpenCL 加载 + 符号绑定 + 平台/设备枚举 + REDCL 构造 + 兼容性检查（内核编译）。
    // 不依赖任何输入素材，供 init() 与 initGpuOnly() 共用。
    bool initOpenCL(std::string& err);

    void* clHandle_ = nullptr;
    R3DSDK::EXT_OCLAPI_1_1 ocl_;
    ClEngine cl_;
    R3DSDK::REDCL* redcl_ = nullptr;
    R3DSDK::AsyncDecoder* adec_ = nullptr;
    R3DSDK::Clip* clip_ = nullptr;
    R3DSDK::ImageProcessingSettings* ip_ = nullptr;

    size_t width_ = 0;
    size_t height_ = 0;
    size_t frameCount_ = 0;
    // GPU 端同时在途（已提交未完成）的 debayer 帧上限；kInFlight 个 raw slot
    // 中最多 kMaxInflight 个处于 GPU 流水线，其余在 ready_/free 流转
    static constexpr size_t kMaxInflight = 6;

    std::vector<Slot> slots_;
    std::deque<Inflight> inflight_;   // FIFO：保序 complete（frames_ 要求帧号递增）
    std::deque<Slot*> free_;
    std::mutex poolM_;
    std::condition_variable poolCv_;

    std::map<size_t, ReadyJob> ready_;
    mutable std::mutex readyM_;
    std::condition_variable readyCv_;
    std::condition_variable doneCv_;
    std::atomic<size_t> outstanding_{0};
    bool workerStop_ = false;
    bool started_ = false;
    std::thread worker_;

    mutable std::string failMsg_;
    mutable std::atomic<bool> fail_{false};
    std::atomic<bool>* abort_ = nullptr;
    size_t startFrame_ = 0;
    FrameQueue* frames_ = nullptr;
    std::string deviceName_;
};

bool GpuPipelineImpl::initOpenCL(std::string& err)
{
    printf("  [1/4] 加载 libOpenCL...\n");
    fflush(stdout);
    const char* envLib = getenv("NRAW_OPENCL_LIB");
    clHandle_ = envLib && *envLib ? dlopen(envLib, RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!clHandle_)
        clHandle_ = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!clHandle_)
        clHandle_ = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
    if (!clHandle_) {
        err = "无法加载 libOpenCL（未安装 OpenCL 运行库，如 Ubuntu 需 ocl-icd-libopencl1）";
        return false;
    }
    if (!bindOpenCL(clHandle_, ocl_)) {
        err = "libOpenCL 缺少所需符号，版本过旧";
        return false;
    }
    printf("  [2/4] 枚举 OpenCL GPU 设备并创建上下文...\n");
    fflush(stdout);
    if (!cl_.init(ocl_, deviceName_, err))
        return false;

    std::string cacheErr;
    std::string cacheDir = openclCacheDir(cacheErr);
    printf("  [3/4] 创建 REDCL（加载 REDOpenCL-x64.so）...\n");
    fflush(stdout);
    try {
        redcl_ = new R3DSDK::REDCL(ocl_, cacheDir.c_str());
    } catch (const std::exception& e) {
        err = std::string("REDCL 构造失败: ") + e.what();
        return false;
    } catch (...) {
        err = "REDCL 构造失败";
        return false;
    }
    printf("OpenCL 设备: %s\n", deviceName_.c_str());
    if (cacheDir.empty())
        printf("OpenCL 内核缓存: 禁用（%s）\n", cacheErr.empty() ? "无缓存目录" : cacheErr.c_str());
    else
        printf("OpenCL 内核缓存: %s\n", cacheDir.c_str());
    printf("  [4/4] 编译 OpenCL 内核（首次可能需数分钟）...\n");
    fflush(stdout);
    cl_int ce = 0;
    R3DSDK::REDCL::Status st = redcl_->checkCompatibility(cl_.context(), cl_.queue(), ce);
    if (st != R3DSDK::REDCL::Status_Ok) {
        err = "GPU 兼容性检查失败 (status " + std::to_string(static_cast<int>(st)) +
              ", clerr " + std::to_string(ce) + ")";
        if (st == R3DSDK::REDCL::Status_UnableToLoadLibrary)
            err += "（无法加载 REDOpenCL 动态库：请确认 REDOpenCL-x64.so 与可执行文件同目录或 --sdk-path 指向正确）";
        return false;
    }
    return true;
}

bool GpuPipelineImpl::initGpuOnly(std::string& err)
{
    return initOpenCL(err);
}

bool GpuPipelineImpl::init(const CliOptions& opt, const MediaInfo& info, std::string& err)
{
    if (!initOpenCL(err))
        return false;

    cl_int ce = 0;
    try {
        clip_ = new R3DSDK::Clip(opt.input.c_str());
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    if (clip_->Status() != R3DSDK::LSClipLoaded) {
        err = "GPU 路径无法打开剪辑 (status " +
              std::to_string(static_cast<int>(clip_->Status())) + ")";
        return false;
    }
    ip_ = static_cast<R3DSDK::ImageProcessingSettings*>(sharedIpSettings());
    if (!ip_) {
        err = "缺少共享 IPP2 设置";
        return false;
    }

    width_ = info.width;
    height_ = info.height;
    frameCount_ = info.frameCount;
    if (width_ == 0 || height_ == 0) {
        err = "无效的分辨率";
        return false;
    }

    try {
        adec_ = new R3DSDK::AsyncDecoder();
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    if (R3DSDK::AsyncDecoder::ThreadsAvailable() == 0) {
        err = "AsyncDecoder 不可用（ThreadsAvailable() == 0）";
        return false;
    }
    adec_->Open(0);

    R3DSDK::AsyncDecompressJob probe;
    probe.Clip = clip_;
    probe.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
    size_t rawSize = R3DSDK::AsyncDecoder::GetSizeBufferNeeded(probe);
    if (rawSize == 0) {
        err = "GetSizeBufferNeeded 返回 0（模式不受支持）";
        return false;
    }

    try {
        slots_.resize(kInFlight);
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    for (auto& s : slots_) {
        s.raw.resize(rawSize, 64);
        if (!s.raw.data()) {
            err = "内存不足 (OOM)";
            return false;
        }
        s.rawSize = rawSize;
        // 每帧独立 GPU raw 缓冲（processAsync 多帧在途必需）
        s.rawDev = cl_.ocl()->clCreateBuffer(cl_.context(), CL_MEM_READ_WRITE,
                                             rawSize, nullptr, &ce);
        if (!s.rawDev || ce != CL_SUCCESS) {
            err = "GPU 原始缓冲分配失败 (clerr " + std::to_string(ce) + ")";
            return false;
        }
        s.cb.pipe = this;
        s.cb.slot = &s;
        s.job.Clip = clip_;
        s.job.VideoTrackNo = 0;
        s.job.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
        s.job.OutputBuffer = s.raw.data();
        s.job.OutputBufferSize = rawSize;
        s.job.AbortDecode = false;
        s.job.OutputFrameMetadata = nullptr;
        s.job.Callback = &GpuPipelineImpl::asyncCb;
        s.job.PrivateData = &s.cb;
        free_.push_back(&s);
    }
    return true;
}

bool GpuPipelineImpl::submit(size_t frameNo, std::string& err)
{
    if (fail_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(readyM_);
        err = failMsg_;
        return false;
    }
    if (frameNo >= frameCount_) {
        err = "帧号越界: " + std::to_string(frameNo);
        return false;
    }
    Slot* s = acquire();
    s->cb.frameNo = frameNo;
    s->job.VideoFrameNo = frameNo;
    s->job.OutputBuffer = s->raw.data();
    s->job.OutputBufferSize = s->rawSize;
    {
        std::lock_guard<std::mutex> lk(readyM_);
        s->abandoned = false;
        ++outstanding_;
    }
    R3DSDK::DecodeStatus st = adec_->DecodeForGpuSdk(s->job);
    if (st != R3DSDK::DSDecodeOK) {
        {
            std::lock_guard<std::mutex> lk(readyM_);
            --outstanding_;
            s->abandoned = true;
            ready_.erase(frameNo);
        }
        release(s);
        setFail("AsyncDecoder 提交失败 (status " + std::to_string(static_cast<int>(st)) +
                ") at frame " + std::to_string(frameNo));
        std::lock_guard<std::mutex> lk(readyM_);
        err = failMsg_;
        return false;
    }
    return true;
}

bool GpuPipelineImpl::decodeSync(size_t frameNo, VideoFrame& out, std::string& err)
{
    if (started_) {
        err = "decodeSync 只能在 start() 之前使用";
        return false;
    }
    if (!submit(frameNo, err))
        return false;
    ReadyJob rj;
    {
        std::unique_lock<std::mutex> lk(readyM_);
        doneCv_.wait(lk, [&] { return ready_.count(frameNo) != 0 ||
                                      fail_.load(std::memory_order_relaxed); });
        if (fail_.load(std::memory_order_relaxed)) {
            err = failMsg_;
            return false;
        }
        auto it = ready_.find(frameNo);
        if (it == ready_.end()) {
            err = "内部错误: 缺少帧 " + std::to_string(frameNo) + " 的回调";
            return false;
        }
        rj = std::move(it->second);
        ready_.erase(it);
        --outstanding_;
    }
    if (rj.status != R3DSDK::DSDecodeOK) {
        release(rj.slot);
        setFail("异步解码失败 (status " + std::to_string(static_cast<int>(rj.status)) +
                ") at frame " + std::to_string(frameNo));
        std::lock_guard<std::mutex> lk(readyM_);
        err = failMsg_;
        return false;
    }
    std::string derr;
    bool ok = debayerStart(rj.slot, derr);
    if (ok)
        ok = debayerComplete(rj.slot, out, derr);
    if (!ok)
        err = derr;
    release(rj.slot);
    if (!ok)
        setFail(err);
    return ok;
}

bool GpuPipelineImpl::start(FrameQueue& frames, std::atomic<bool>* abort,
                              size_t startFrame, std::string& err)
{
    if (!adec_) {
        err = "管线未初始化";
        return false;
    }
    if (started_)
        return true;
    frames_ = &frames;
    abort_ = abort;
    startFrame_ = startFrame;
    {
        std::lock_guard<std::mutex> lk(readyM_);
        fail_.store(false, std::memory_order_relaxed);
        failMsg_.clear();
    }
    started_ = true;
    try {
        worker_ = std::thread(&GpuPipelineImpl::worker, this);
    } catch (const std::exception& e) {
        started_ = false;
        frames_ = nullptr;
        abort_ = nullptr;
        err = std::string("创建 GPU 工作线程失败: ") + e.what();
        return false;
    }
    return true;
}

bool GpuPipelineImpl::finish(std::string& err)
{
    if (!started_) {
        err = "管线未启动";
        return false;
    }
    {
        std::unique_lock<std::mutex> lk(readyM_);
        doneCv_.wait(lk, [&] { return outstanding_.load() == 0; });
    }
    {
        std::lock_guard<std::mutex> lk(readyM_);
        workerStop_ = true;
    }
    readyCv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (frames_)
        frames_->setEof();
    if (fail_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(readyM_);
        err = failMsg_;
        return false;
    }
    return true;
}

void GpuPipelineImpl::close()
{
    if (frames_)
        frames_->setEof();
    if (worker_.joinable()) {
        {
            std::unique_lock<std::mutex> lk(readyM_);
            doneCv_.wait(lk, [&] { return outstanding_.load() == 0; });
        }
        {
            std::lock_guard<std::mutex> lk(readyM_);
            workerStop_ = true;
        }
        readyCv_.notify_all();
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lk(readyM_);
        ready_.clear();
        workerStop_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(poolM_);
        free_.clear();
    }
    if (frames_) {
        frames_->setEof();
        frames_ = nullptr;
    }
    abort_ = nullptr;
    ip_ = nullptr;
    if (adec_) {
        adec_->Close();
        delete adec_;
        adec_ = nullptr;
    }
    // 异常/失败路径可能残留未完成的 in-flight 帧：completeAsync 释放 SDK 资源
    for (auto& ifl : inflight_) {
        if (ifl.job && redcl_) {
            ifl.job->completeAsync();
            redcl_->releaseDebayerJob(ifl.job);
        }
    }
    inflight_.clear();
    if (redcl_) {
        delete redcl_;
        redcl_ = nullptr;
    }
    if (cl_.ocl()) {
        for (auto& s : slots_) {
            if (s.outDev) {
                cl_.ocl()->clReleaseMemObject(s.outDev);
                s.outDev = nullptr;
            }
            if (s.rawDev) {
                cl_.ocl()->clReleaseMemObject(s.rawDev);
                s.rawDev = nullptr;
            }
        }
    }
    cl_.destroy();
    if (clip_) {
        delete clip_;
        clip_ = nullptr;
    }
    if (clHandle_) {
        dlclose(clHandle_);
        clHandle_ = nullptr;
    }
}

GpuPipelineImpl::Slot* GpuPipelineImpl::acquire()
{
    std::unique_lock<std::mutex> lk(poolM_);
    poolCv_.wait(lk, [&] { return !free_.empty(); });
    Slot* s = free_.back();
    free_.pop_back();
    return s;
}

void GpuPipelineImpl::release(Slot* s)
{
    std::lock_guard<std::mutex> lk(poolM_);
    free_.push_back(s);
    poolCv_.notify_one();
}

bool GpuPipelineImpl::debayerStart(Slot* s, std::string& err)
{
    R3DSDK::EXT_OCLAPI_1_1* ocl = cl_.ocl();
    // 上传用 CL_FALSE（异步入队）：同一 in-order 队列上 processAsync 会
    // 在上传之后执行；raw 缓冲在 completeAsync 前不会释放，数据安全
    if (ocl->clEnqueueWriteBuffer(cl_.queue(), s->rawDev, CL_FALSE, 0, s->rawSize,
                                  s->raw.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
        err = "raw 数据上传 GPU 失败";
        return false;
    }
    R3DSDK::DebayerOpenCLJob* job = redcl_->createDebayerJob();
    if (!job) {
        err = "createDebayerJob 失败";
        return false;
    }
    job->raw_host_mem = s->raw.data();
    job->raw_device_mem = s->rawDev;
    job->mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
    job->imageProcessingSettings = ip_;
    job->pixelType = R3DSDK::PixelType_16Bit_RGB_Interleaved;
    if (!s->outDev) {
        s->outSize = R3DSDK::DebayerOpenCLJob::ResultFrameSize(*job);
        if (s->outSize == 0) {
            redcl_->releaseDebayerJob(job);
            err = "ResultFrameSize 返回 0（原始帧数据无效）";
            return false;
        }
        cl_int ce = 0;
        s->outDev = ocl->clCreateBuffer(cl_.context(), CL_MEM_READ_WRITE, s->outSize,
                                        nullptr, &ce);
        if (!s->outDev || ce != CL_SUCCESS) {
            redcl_->releaseDebayerJob(job);
            err = "GPU 输出缓冲分配失败 (clerr " + std::to_string(ce) + ")";
            return false;
        }
        s->outHost.resize(s->outSize, 64);
        if (!s->outHost.data()) {
            redcl_->releaseDebayerJob(job);
            err = "内存不足 (OOM)";
            return false;
        }
    }
    job->output_device_mem_size = s->outSize;
    job->output_device_mem = s->outDev;
    cl_int ce = 0;
    // 异步提交：不等待 GPU，立即返回，帧进入 inflight_ 由 worker 保序 complete
    R3DSDK::REDCL::Status st = redcl_->processAsync(cl_.context(), cl_.queue(), job, ce);
    if (st != R3DSDK::REDCL::Status_Ok) {
        redcl_->releaseDebayerJob(job);
        err = "REDCL 异步处理失败 (status " + std::to_string(static_cast<int>(st)) +
              ", clerr " + std::to_string(ce) + ")";
        return false;
    }
    inflight_.push_back(Inflight{s, job, s->cb.frameNo});
    return true;
}

bool GpuPipelineImpl::debayerComplete(Slot* s, VideoFrame& out, std::string& err)
{
    // 从 inflight_ 取最旧帧完成（FIFO 保序：frames_ 要求帧号严格递增）
    if (inflight_.empty()) {
        err = "内部错误: inflight 队列为空";
        return false;
    }
    Inflight ifl = inflight_.front();
    if (ifl.slot != s) {
        err = "内部错误: debayer 完成顺序错乱";
        return false;
    }
    inflight_.pop_front();
    R3DSDK::EXT_OCLAPI_1_1* ocl = cl_.ocl();
    // 阻塞直到该帧的 GPU 工作（含上传与 REDCL 处理）完成，并释放 SDK 内部资源
    ifl.job->completeAsync();
    redcl_->releaseDebayerJob(ifl.job);
    if (ocl->clEnqueueReadBuffer(cl_.queue(), s->outDev, CL_TRUE, 0, s->outSize,
                                 s->outHost.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
        err = "GPU 结果回读失败";
        return false;
    }
    out.width = width_;
    out.height = height_;
    out.frameNo = s->cb.frameNo;
    try {
        out.rgb.resize(width_ * height_ * 6, 16);
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    if (!out.rgb.data()) {
        err = "内存不足 (OOM)";
        return false;
    }
    const uint16_t* src = static_cast<const uint16_t*>(s->outHost.data());
    uint16_t* dst = static_cast<uint16_t*>(out.rgb.data());
    const size_t n = width_ * height_;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[3 * i];
        dst[n + i] = src[3 * i + 1];
        dst[2 * n + i] = src[3 * i + 2];
    }
    return true;
}

void GpuPipelineImpl::worker()
{
    // 起点自适应：续传时 submit 从 resumeFrame（>0）开始，ready_ 里不会出现
    // 帧 0——若硬编码 nextPush=0 会永远不命中 → 三方死锁（worker 等 ready、
    // 主线程等 slot、编码线程等帧）。首个到达的 ready 条目即起点。
    size_t nextPush = startFrame_;
    for (;;) {
        // 阶段 A：把下一帧提交到 GPU（processAsync，不等待）。
        // 正常时 inflight 保持 ≤ kMaxInflight；收尾（workerStop_）时允许超限，
        // 把 ready_ 剩余帧全部提交后统一完成。
        ReadyJob rj;
        bool got = false;
        {
            std::unique_lock<std::mutex> lk(readyM_);
            if (!ready_.empty() && ready_.begin()->first == nextPush &&
                (inflight_.size() < kMaxInflight || workerStop_)) {
                auto it = ready_.begin();
                rj = std::move(it->second);
                ready_.erase(it);
                got = true;
            }
        }
        if (got) {
            if (rj.status != R3DSDK::DSDecodeOK) {
                setFail("异步解码失败 (status " +
                        std::to_string(static_cast<int>(rj.status)) +
                        ") at frame " + std::to_string(rj.frameNo));
                ++nextPush;
                release(rj.slot);
                {
                    std::lock_guard<std::mutex> lk(readyM_);
                    --outstanding_;
                }
                doneCv_.notify_all();
                continue;
            }
            std::string err;
            if (!debayerStart(rj.slot, err)) {
                setFail(err);
                ++nextPush;
                release(rj.slot);
                {
                    std::lock_guard<std::mutex> lk(readyM_);
                    --outstanding_;
                }
                doneCv_.notify_all();
                continue;
            }
            // 提交成功即推进 nextPush：允许继续提交下一帧（多帧在途，
            // kMaxInflight 重叠 debayer/complete 才生效；否则恒 ≤1 串行）。
            // 保序由 inflight_ 队列保证（Phase B 总是完成最旧）。
            ++nextPush;
            continue;  // 已入 inflight_，继续提交更多帧
        }
        // 阶段 B：完成最旧 inflight 帧（completeAsync + 回读 + push，保序）
        if (!inflight_.empty()) {
            Slot* slot = inflight_.front().slot;
            std::string err;
            VideoFrame f;
            if (!debayerComplete(slot, f, err)) {
                setFail(err);
            } else {
                try {
                    frames_->push(std::make_unique<VideoFrame>(std::move(f)));
                } catch (const std::bad_alloc&) {
                    setFail("内存不足 (OOM)");
                }
            }
            release(slot);
            {
                std::lock_guard<std::mutex> lk(readyM_);
                --outstanding_;
            }
            doneCv_.notify_all();
            continue;
        }
        // 阶段 C：无事可做——收尾退出，或等待 nextPush 帧解压完成
        if (workerStop_)
            break;
        {
            std::unique_lock<std::mutex> lk(readyM_);
            readyCv_.wait(lk, [&] {
                return workerStop_ ||
                       (!ready_.empty() && ready_.begin()->first == nextPush);
            });
        }
    }
}

void GpuPipelineImpl::setFail(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(readyM_);
    if (!fail_.load(std::memory_order_relaxed))
        failMsg_ = msg;
    fail_.store(true, std::memory_order_relaxed);
    if (abort_)
        abort_->store(true);
    readyCv_.notify_all();
    doneCv_.notify_all();
}

void GpuPipelineImpl::asyncCb(R3DSDK::AsyncDecompressJob* item, R3DSDK::DecodeStatus st)
{
    if (!item || !item->PrivateData)
        return;
    auto* cb = static_cast<CbData*>(item->PrivateData);
    cb->pipe->onReady(item, st);
}

void GpuPipelineImpl::onReady(R3DSDK::AsyncDecompressJob* item, R3DSDK::DecodeStatus st)
{
    if (!item || !item->PrivateData)
        return;
    auto* cb = static_cast<CbData*>(item->PrivateData);
    {
        std::lock_guard<std::mutex> lk(readyM_);
        if (cb->slot->abandoned)
            return;
        try {
            ready_[cb->frameNo] = ReadyJob{cb->slot, cb->frameNo, st};
        } catch (const std::bad_alloc&) {
            if (!fail_.load(std::memory_order_relaxed))
                failMsg_ = "内存不足 (OOM)";
            fail_.store(true, std::memory_order_relaxed);
            if (abort_)
                abort_->store(true);
            --outstanding_;
            release(cb->slot);
            readyCv_.notify_all();
            doneCv_.notify_all();
            return;
        }
    }
    readyCv_.notify_all();
    doneCv_.notify_all();
}

GpuPipeline::GpuPipeline()
{
    impl_ = new GpuPipelineImpl();
}

GpuPipeline::~GpuPipeline()
{
    close();
    delete static_cast<GpuPipelineImpl*>(impl_);
    impl_ = nullptr;
}

bool GpuPipeline::init(const CliOptions& opt, const MediaInfo& info, std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->init(opt, info, err);
}

bool GpuPipeline::initGpuOnly(std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->initGpuOnly(err);
}

bool GpuPipeline::decodeSync(size_t frameNo, VideoFrame& out, std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->decodeSync(frameNo, out, err);
}

bool GpuPipeline::start(FrameQueue& frames, std::atomic<bool>* abort,
                          size_t startFrame, std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->start(frames, abort, startFrame, err);
}

bool GpuPipeline::submit(size_t frameNo, std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->submit(frameNo, err);
}

bool GpuPipeline::finish(std::string& err)
{
    return static_cast<GpuPipelineImpl*>(impl_)->finish(err);
}

void GpuPipeline::close()
{
    static_cast<GpuPipelineImpl*>(impl_)->close();
}

const std::string& GpuPipeline::deviceName() const
{
    return static_cast<GpuPipelineImpl*>(impl_)->deviceName();
}

std::string GpuPipeline::lastError() const
{
    return static_cast<GpuPipelineImpl*>(impl_)->lastError();
}
}
