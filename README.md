```
external_components:
  - source: local
    components: [movie]

movie:
  id: my_movie_player
  display_width: 320
  display_height: 240
  buffer_size: 16384
  fps: 15
  http_timeout: 5000
  http_buffer_size: 8192

button:
  - platform: template
    name: "Play Video from HTTP"
    on_press:
      - lambda: |-
          id(my_movie_player).play_http_stream("http://192.168.1.100:8080/video.mjpeg", movie::VIDEO_FORMAT_MJPEG);
```
