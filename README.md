# lossylab

**lossylab** is a C++ library with Python bindings that uses FFmpeg to model, apply and inspect the processing history of images and video frames. It serves two purposes that share the same machinery.
First, it reproduces realistic real-world degradations for deep learning augmentation pipelines: resampling, chroma subsampling, color-space mix-ups, filtering, and multi-generation image and video compression.
Second, it audits existing data by uncovering the traces that encoders, platforms and post-processing leave behind.
Every operation is explicit, with no hidden format or color conversions, and returns a structured processing record. That makes each transformation reproducible, traceable to mask and crop coordinates, and comparable across data sources.
