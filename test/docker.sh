#!/bin/bash
cd /home/hoene/git/vlc/test/docker
# sudo docker build -t hoene/vlc:latest .
sudo docker run -p 5901:5901 -v /home/hoene/git/vlc:/root/vlc -v /dev/input:/dev/input -v /dev/bus/usb:/dev/bus/usb --privileged -v /home/hoene/git/SOFAlizer:/root/SOFAlizer --device /dev/sr0:/dev/sr0 -i  -e "USER=root" -e "PULSE_SERVER=tcp:172.17.42.1" --rm hoene/vlc:latest bash -c "touch /root/.Xresources && vncserver :1 -geometry 1280x800 -depth 24 && tail -F /root/.vnc/*.log"


#sudo docker run -p 5901:5901 -v /home/hoene/git/vlc:/root/vlc -v /dev/input:/dev/input -v /dev/bus/usb:/dev/bus/usb --privileged -v /home/hoene/git/SOFAlizer:/root/SOFAlizer --device /dev/sr0:/dev/sr0 -i  -e "USER=root"  -v /dev/sndt:/dev/snd --rm hoene/vlc:latest bash -c "touch /root/.Xresources && vncserver :1 -geometry 1280x800 -depth 24 && tail -F /root/.vnc/*.log"

# docker run --rm --name chromium --device /dev/snd -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -v /run/dbus/:/run/dbus/:rw -v /dev/shm:/dev/shm <myCustomChromiumImage>

# docker run -it --rm -p 5901:5901 -p4713 -e USER=root debian-desktop bash -c "touch /root/.Xresources && vncserver :1 -geometry 1280x800 -depth 24 && tail -F /root/.vnc/*.log"
