```
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/movie
    components: [video_camera, display_helper]
    refresh: 0s 

video_camera:
  id: stream
  url: rtsp://
  fps: 1

display_helper:
  id: rtsp_display
  camera_id: stream
  display_id: my_display
  width: 320
  height: 240
```
