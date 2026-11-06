#include "engine.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;
using namespace fx3;
PYBIND11_MODULE(_native, m) {
    m.doc() = "Native queued CyAPI acquisition; no Python callbacks on the acquisition thread";
    py::class_<DeviceInfo>(m, "DeviceInfo")
        .def_readonly("index", &DeviceInfo::index)
        .def_readonly("vendor_id", &DeviceInfo::vendor_id)
        .def_readonly("product_id", &DeviceInfo::product_id)
        .def_readonly("name", &DeviceInfo::name)
        .def_readonly("serial", &DeviceInfo::serial);
    m.def("list_devices", &list_devices, py::call_guard<py::gil_scoped_release>());
    py::class_<Config>(m, "AcquisitionConfig")
        .def(py::init<>())
        .def_readwrite("block_size", &Config::block_size)
        .def_readwrite("queue_depth", &Config::queue_depth)
        .def_readwrite("queue_capacity", &Config::queue_capacity)
        .def_readwrite("pool_extra", &Config::pool_extra)
        .def_readwrite("timeout_ms", &Config::timeout_ms)
        .def_readwrite("simulated_interval_ms", &Config::simulated_interval_ms)
        .def_readwrite("simulated_fail_after", &Config::simulated_fail_after);
    py::class_<Block, std::shared_ptr<Block>>(m, "DataBlock", py::buffer_protocol())
        .def_readonly("size", &Block::size)
        .def_readonly("sequence", &Block::sequence)
        .def_readonly("timestamp_ns", &Block::timestamp_ns)
        .def_buffer([](Block& b) {
            return py::buffer_info(b.buffer->bytes.data(), sizeof(uint8_t),
                py::format_descriptor<uint8_t>::format(), 1,
                {static_cast<py::ssize_t>(b.size)}, {1}, true);
        });
    py::class_<Stats>(m, "AcquisitionStats")
        .def_readonly("opened", &Stats::opened)
        .def_readonly("running", &Stats::running)
        .def_readonly("received_blocks", &Stats::received_blocks)
        .def_readonly("received_bytes", &Stats::received_bytes)
        .def_readonly("dropped_blocks", &Stats::dropped_blocks)
        .def_readonly("dropped_bytes", &Stats::dropped_bytes)
        .def_readonly("errors", &Stats::errors)
        .def_readonly("queued_blocks", &Stats::queued_blocks)
        .def_readonly("queue_high_water", &Stats::queue_high_water)
        .def_readonly("pool_blocks", &Stats::pool_blocks)
        .def_readonly("free_buffers", &Stats::free_buffers)
        .def_readonly("pool_bytes", &Stats::pool_bytes)
        .def_readonly("elapsed_seconds", &Stats::elapsed_seconds)
        .def_readonly("last_error", &Stats::last_error);
    py::class_<Engine>(m, "AcquisitionEngine")
        .def(py::init<>())
        .def("open", &Engine::open, py::arg("index") = 0, py::arg("endpoint") = 0x81,
             py::arg("simulated") = false, py::call_guard<py::gil_scoped_release>())
        .def("configure", [](Engine& e, const Config& config) {
            const Config snapshot = config;
            py::gil_scoped_release release;
            e.configure(snapshot);
        }, py::arg("config"))
        .def("start", &Engine::start, py::call_guard<py::gil_scoped_release>())
        .def("read_block", &Engine::read_block, py::arg("timeout_ms") = 100,
             py::call_guard<py::gil_scoped_release>())
        .def("stats", &Engine::stats, py::call_guard<py::gil_scoped_release>())
        .def("stop", &Engine::stop, py::call_guard<py::gil_scoped_release>())
        .def("close", &Engine::close, py::call_guard<py::gil_scoped_release>())
        .def("__enter__", [](Engine& e) -> Engine& { return e; }, py::return_value_policy::reference_internal)
        .def("__exit__", [](Engine& e, py::object, py::object, py::object) {
            py::gil_scoped_release release; e.close();
        });
}
