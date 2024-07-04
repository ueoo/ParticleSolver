
mkdir build
cd build
make clean && qmake ../cpu/particles.pro && make -j64
cd ..
