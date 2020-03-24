#define KEEP_CL_CHECK
#include "frt.h"

#include <cstring>

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <CL/cl_ext.h>
#include <tinyxml.h>
#include <xclbin.h>
#include <CL/cl2.hpp>

using std::clog;
using std::endl;
using std::runtime_error;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

namespace fpga {

namespace internal {
string Exec(const char* cmd) {
  std::string result;
  unique_ptr<FILE, decltype(&pclose)> pipe{popen(cmd, "r"), pclose};
  if (pipe == nullptr) {
    throw runtime_error(string{"cannot execute: "} + cmd);
  }
  char c;
  while (c = fgetc(pipe.get()), !feof(pipe.get())) {
    result += c;
  }
  return result;
}
}  // namespace internal

cl::Program::Binaries LoadBinaryFile(const string& file_name) {
  clog << "INFO: Loading " << file_name << endl;
  std::ifstream stream(file_name, std::ios::binary);
  return {{std::istreambuf_iterator<char>(stream),
           std::istreambuf_iterator<char>()}};
}

Instance::Instance(const string& bitstream) {
  cl::Program::Binaries binaries = LoadBinaryFile(bitstream);
  string target_device_name;
  string vendor_name;
  string kernel_name;
  for (const auto& binary : binaries) {
    if (binary.size() >= 8 && memcmp(binary.data(), "xclbin2", 8) == 0) {
      vendor_name = "Xilinx";
      const auto axlf_top = reinterpret_cast<const axlf*>(binary.data());
      switch (axlf_top->m_header.m_mode) {
        case XCLBIN_FLAT:
        case XCLBIN_PR:
        case XCLBIN_TANDEM_STAGE2:
        case XCLBIN_TANDEM_STAGE2_WITH_PR:
          break;
        case XCLBIN_HW_EMU:
          setenv("XCL_EMULATION_MODE", "hw_emu", 0);
          break;
        case XCLBIN_SW_EMU:
          setenv("XCL_EMULATION_MODE", "sw_emu", 0);
          break;
        default:
          throw runtime_error("unknown xclbin mode");
      }
      target_device_name =
          reinterpret_cast<const char*>(axlf_top->m_header.m_platformVBNV);
      auto metadata = xclbin::get_axlf_section(axlf_top, EMBEDDED_METADATA);
      if (metadata != nullptr) {
        TiXmlDocument doc;
        doc.Parse(
            reinterpret_cast<const char*>(axlf_top) + metadata->m_sectionOffset,
            0, TIXML_ENCODING_UTF8);
        auto xml_core = doc.FirstChildElement("project")
                            ->FirstChildElement("platform")
                            ->FirstChildElement("device")
                            ->FirstChildElement("core");
        auto xml_kernel = xml_core->FirstChildElement("kernel");
        kernel_name = xml_kernel->Attribute("name");
        string target_meta = xml_core->Attribute("target");
        for (auto xml_arg = xml_kernel->FirstChildElement("arg");
             xml_arg != nullptr; xml_arg = xml_arg->NextSiblingElement("arg")) {
          auto index = atoi(xml_arg->Attribute("id"));
          auto& arg = arg_table_[index];
          arg.index = index;
          arg.name = xml_arg->Attribute("name");
          arg.type = xml_arg->Attribute("type");
          auto cat = atoi(xml_arg->Attribute("addressQualifier"));
          switch (cat) {
            case 0:
              arg.cat = ArgInfo::kScalar;
              break;
            case 1:
              arg.cat = ArgInfo::kMmap;
              break;
            case 4:
              arg.cat = ArgInfo::kStream;
              break;
            default:
              clog << "WARNING: Unknown argument category: " << cat;
          }
        }
        // m_mode doesn't always work
        if (target_meta == "hw_em") {
          setenv("XCL_EMULATION_MODE", "hw_emu", 0);
        } else if (target_meta == "csim") {
          setenv("XCL_EMULATION_MODE", "sw_emu", 0);
        }
      } else {
        throw runtime_error("cannot determine kernel name from binary");
      }
      unordered_map<int32_t, string> memory_table;
      auto mem_info = xclbin::get_axlf_section(axlf_top, MEM_TOPOLOGY);
      if (mem_info != nullptr) {
        auto topology = reinterpret_cast<const mem_topology*>(
            reinterpret_cast<const char*>(axlf_top) +
            mem_info->m_sectionOffset);
        for (int i = 0; i < topology->m_count; ++i) {
          const mem_data& mem = topology->m_mem_data[i];
          if (mem.m_used) {
            memory_table[i] = reinterpret_cast<const char*>(mem.m_tag);
          }
        }
      }
      auto conn = xclbin::get_axlf_section(axlf_top, CONNECTIVITY);
      if (conn != nullptr) {
        auto connect = reinterpret_cast<const connectivity*>(
            reinterpret_cast<const char*>(axlf_top) + conn->m_sectionOffset);
        for (int i = 0; i < connect->m_count; ++i) {
          const connection& c = connect->m_connection[i];
          arg_table_[c.arg_index].tag = memory_table[c.mem_data_index];
        }
      }
    } else {
      throw runtime_error("unexpected bitstream file");
    }
  }
  if (getenv("XCL_EMULATION_MODE")) {
    string cmd =
        R"([ "$(jq -r '.Platform.Boards[]|select(.Devices[]|select(.Name==")" +
        target_device_name +
        R"R("))' emconfig.json 2>/dev/null)" != "" ] || )R"
        "emconfigutil --platform " +
        target_device_name;
    if (system(cmd.c_str())) {
      throw std::runtime_error("emconfigutil failed");
    }
    const char* tmpdir = getenv("TMPDIR");
    setenv("SDACCEL_EM_RUN_DIR", tmpdir ? tmpdir : "/tmp", 0);
  }

  const char* xcl_emulation_mode = getenv("XCL_EMULATION_MODE");
  if (xcl_emulation_mode != nullptr && string{"sw_emu"} == xcl_emulation_mode) {
    string ld_library_path;
    if (auto xilinx_sdx = getenv("XILINX_VITIS")) {
      // find LD_LIBRARY_PATH by sourcing ${XILINX_VITIS}/settings64.sh
      ld_library_path = internal::Exec(
          R"(bash -c '. "${XILINX_VITIS}/settings64.sh" && )"
          R"(printf "${LD_LIBRARY_PATH}"')");
    } else {
      // find XILINX_VITIS and LD_LIBRARY_PATH with vivado_hls
      // ld_library_path is null-separated string of both env vars
      ld_library_path = internal::Exec(
          R"(bash -c '. "$(vivado_hls -r -l /dev/null | grep "^/"))"
          R"(/settings64.sh" && printf "${LD_LIBRARY_PATH}\0${XILINX_VITIS}"')");
      setenv("XILINX_VITIS",
             ld_library_path.c_str() + strlen(ld_library_path.c_str()) + 1, 1);
    }
    if (auto xilinx_sdx = getenv("XILINX_SDX")) {
      // find LD_LIBRARY_PATH by sourcing ${XILINX_SDX}/settings64.sh
      ld_library_path = internal::Exec(
          R"(bash -c '. "${XILINX_SDX}/settings64.sh" && )"
          R"(printf "${LD_LIBRARY_PATH}"')");
    } else {
      // find XILINX_SDX and LD_LIBRARY_PATH with vivado_hls
      // ld_library_path is null-separated string of both env vars
      ld_library_path = internal::Exec(
          R"(bash -c '. "$(vivado_hls -r -l /dev/null | grep "^/"))"
          R"(/settings64.sh" && printf "${LD_LIBRARY_PATH}\0${XILINX_SDX}"')");
      setenv("XILINX_SDX",
             ld_library_path.c_str() + strlen(ld_library_path.c_str()) + 1, 1);
    }
    setenv("LD_LIBRARY_PATH", ld_library_path.c_str(), 1);
  }
  vector<cl::Platform> platforms;
  CL_CHECK(cl::Platform::get(&platforms));
  cl_int err;
  for (const auto& platform : platforms) {
    string platformName = platform.getInfo<CL_PLATFORM_NAME>(&err);
    CL_CHECK(err);
    clog << "INFO: Found platform: " << platformName.c_str() << endl;
    if (platformName == vendor_name) {
      vector<cl::Device> devices;
      CL_CHECK(platform.getDevices(CL_DEVICE_TYPE_ACCELERATOR, &devices));
      for (const auto& device : devices) {
        const string device_name = device.getInfo<CL_DEVICE_NAME>(&err);
        CL_CHECK(err);
        clog << "INFO: Found device: " << device_name << endl;
        if (device_name == target_device_name) {
          clog << "INFO: Using " << device_name << endl;
          device_ = device;
          context_ = cl::Context(device, nullptr, nullptr, nullptr, &err);
          if (err == CL_DEVICE_NOT_AVAILABLE) {
            continue;
          }
          CL_CHECK(err);
          cmd_ = cl::CommandQueue(context_, device,
                                  CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE |
                                      CL_QUEUE_PROFILING_ENABLE,
                                  &err);
          CL_CHECK(err);
          vector<int> binary_status;
          program_ =
              cl::Program(context_, {device}, binaries, &binary_status, &err);
          for (auto status : binary_status) {
            CL_CHECK(status);
          }
          CL_CHECK(err);
          kernel_ = cl::Kernel(program_, kernel_name.c_str(), &err);
          CL_CHECK(err);
          return;
        }
      }
    }
  }
}

cl::Buffer Instance::CreateBuffer(int index, cl_mem_flags flags, size_t size,
                                  void* host_ptr) {
  cl_mem_ext_ptr_t ext;
  if (arg_table_.count(index)) {
    unordered_map<string, decltype(XCL_MEM_DDR_BANK0)> kTagTable{
        {"bank0", XCL_MEM_DDR_BANK0},  {"bank1", XCL_MEM_DDR_BANK1},
        {"bank2", XCL_MEM_DDR_BANK2},  {"bank3", XCL_MEM_DDR_BANK3},
        {"DDR[0]", XCL_MEM_DDR_BANK0}, {"DDR[1]", XCL_MEM_DDR_BANK1},
        {"DDR[2]", XCL_MEM_DDR_BANK2}, {"DDR[3]", XCL_MEM_DDR_BANK3},
    };
    for (int i = 0; i < 32; ++i) {
      kTagTable["HBM[" + std::to_string(i) + "]"] = i | XCL_MEM_TOPOLOGY;
    }
    ext.flags = 0;
    auto flag = kTagTable.find(arg_table_[index].tag);
    if (flag != kTagTable.end()) {
      ext.flags = flag->second;
#ifndef NDEBUG
      clog << "DEBUG: Argument " << index << " assigned to "
           << arg_table_[index].tag << endl;
#endif
    } else if (!arg_table_[index].tag.empty()) {
      clog << "WARNING: Unknown argument memory tag: " << arg_table_[index].tag
           << endl;
    }
    ext.obj = host_ptr;
    ext.param = nullptr;
    flags |= CL_MEM_EXT_PTR_XILINX;
    host_ptr = &ext;
  }
  cl_int err;
  auto buffer = cl::Buffer(context_, flags, size, host_ptr, &err);
  CL_CHECK(err);
  buffer_table_[index] = buffer;
  return buffer;
}

void Instance::WriteToDevice() {
  if (!load_buffers_.empty()) {
    load_event_.resize(1);
    CL_CHECK(cmd_.enqueueMigrateMemObjects(load_buffers_, /* flags = */ 0,
                                           nullptr, load_event_.data()));
  }
}

void Instance::ReadFromDevice() {
  if (!store_buffers_.empty()) {
    store_event_.resize(1);
    CL_CHECK(cmd_.enqueueMigrateMemObjects(
        store_buffers_, CL_MIGRATE_MEM_OBJECT_HOST, &compute_event_,
        store_event_.data()));
  }
}

void Instance::Exec() {
  compute_event_.resize(1);
  CL_CHECK(cmd_.enqueueNDRangeKernel(kernel_, cl::NullRange, cl::NDRange(1),
                                     cl::NDRange(1), &load_event_,
                                     compute_event_.data()));
}

void Instance::Finish() {
  CL_CHECK(cmd_.flush());
  CL_CHECK(cmd_.finish());
}

template <cl_profiling_info name>
cl_ulong ProfilingInfo(const cl::Event& event) {
  cl_int err;
  auto time = event.getProfilingInfo<name>(&err);
  CL_CHECK(err);
  return time;
}
template <cl_profiling_info name>
cl_ulong Instance::LoadProfilingInfo() {
  if (load_event_.empty()) {
    return 0ULL;
  }
  return ProfilingInfo<name>(load_event_[0]);
}
template <cl_profiling_info name>
cl_ulong Instance::ComputeProfilingInfo() {
  if (compute_event_.empty()) {
    return 0ULL;
  }
  return ProfilingInfo<name>(compute_event_[0]);
}
template <cl_profiling_info name>
cl_ulong Instance::StoreProfilingInfo() {
  if (store_event_.empty()) {
    return 0ULL;
  }
  return ProfilingInfo<name>(store_event_[0]);
}
cl_ulong Instance::LoadTimeNanoSeconds() {
  return LoadProfilingInfo<CL_PROFILING_COMMAND_END>() -
         LoadProfilingInfo<CL_PROFILING_COMMAND_START>();
}
cl_ulong Instance::ComputeTimeNanoSeconds() {
  return ComputeProfilingInfo<CL_PROFILING_COMMAND_END>() -
         ComputeProfilingInfo<CL_PROFILING_COMMAND_START>();
}
cl_ulong Instance::StoreTimeNanoSeconds() {
  return StoreProfilingInfo<CL_PROFILING_COMMAND_END>() -
         StoreProfilingInfo<CL_PROFILING_COMMAND_START>();
}
double Instance::LoadTimeSeconds() { return LoadTimeNanoSeconds() / 1e9; }
double Instance::ComputeTimeSeconds() { return ComputeTimeNanoSeconds() / 1e9; }
double Instance::StoreTimeSeconds() { return StoreTimeNanoSeconds() / 1e9; }
double Instance::LoadThroughputGbps() {
  size_t total_size = 0;
  for (const auto& buffer : load_buffers_) {
    total_size += buffer.getInfo<CL_MEM_SIZE>();
  }
  return double(total_size) / LoadTimeNanoSeconds();
}
double Instance::StoreThroughputGbps() {
  size_t total_size = 0;
  for (const auto& buffer : store_buffers_) {
    total_size += buffer.getInfo<CL_MEM_SIZE>();
  }
  return double(total_size) / StoreTimeNanoSeconds();
}
template cl_ulong Instance::LoadProfilingInfo<CL_PROFILING_COMMAND_QUEUED>();
template cl_ulong Instance::LoadProfilingInfo<CL_PROFILING_COMMAND_SUBMIT>();
template cl_ulong Instance::ComputeProfilingInfo<CL_PROFILING_COMMAND_QUEUED>();
template cl_ulong Instance::ComputeProfilingInfo<CL_PROFILING_COMMAND_SUBMIT>();
template cl_ulong Instance::StoreProfilingInfo<CL_PROFILING_COMMAND_QUEUED>();
template cl_ulong Instance::StoreProfilingInfo<CL_PROFILING_COMMAND_SUBMIT>();

}  // namespace fpga
