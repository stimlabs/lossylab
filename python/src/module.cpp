#include "bindings.hpp"

NB_MODULE(_lossylab, m)
{
    m.doc() = "Model, apply and inspect the processing history of images and video frames.";

    lossylab::pybind::bind_errors(m);
    lossylab::pybind::bind_core_types(m);
    lossylab::pybind::bind_frame(m);
    lossylab::pybind::bind_record(m);
    lossylab::pybind::bind_env(m);
    lossylab::pybind::bind_io(m);
    lossylab::pybind::bind_video_reader(m);
    lossylab::pybind::bind_measure(m);
    lossylab::pybind::bind_convert(m);
    lossylab::pybind::bind_encode(m);
    lossylab::pybind::bind_pipeline(m);
    // After bind_pipeline: capture_run's default record_detail needs RecordDetail bound.
    lossylab::pybind::bind_file_result(m);
}
