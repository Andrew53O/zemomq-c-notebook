# Presentation Animation

This folder contains a Manim scene that explains the project architecture.

Render from the project root:

```bash
manim -qm --media_dir animation/media animation/architecture_animation.py ZeroMQNotebookArchitecture
```

Expected output:

```text
animation/media/videos/architecture_animation/720p30/ZeroMQNotebookArchitecture.mp4
```

The animation shows the execution flow:

```text
Browser UI -> C HTTP Server -> ZeroMQ ROUTER/DEALER Broker -> C Kernel Worker -> Runtime Files -> Browser UI
```
