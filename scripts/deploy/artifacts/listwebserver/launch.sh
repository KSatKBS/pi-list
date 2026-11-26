#!/bin/bash

bash /app/gen_static_config.sh &
service nginx start &
node ./static_server.js /app/gui 8080 &
# Relaunch the API server if it crashes (otherwise nginx answers 502 to every API call)
(while true; do
    node ./dist/server.js config.yml --dev
    echo "listwebserver exited with code $?, restarting in 2 s"
    sleep 2
done) &

# Relaunch if it crashes
while true; do ../bin/stream_pre_processor amqp://rabbitmq:5672 ; done
