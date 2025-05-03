```
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/movie
    components: [movie]
    refresh: 0s 

movie:
  id: my_movie_player
  display: my_display
  width: 128
  height: 64
  buffer_size: 16384
  fps: 15
  http_timeout: 10000
  format: MJPEG

button:
  - platform: template
    name: "Play Video from HTTP"
    on_press:
      - lambda: |-
          id(my_movie_player).play_http_stream("http://192.168.1.100:8080/video.mjpeg", movie::VIDEO_FORMAT_MJPEG);
```
