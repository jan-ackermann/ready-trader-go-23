rm ./build/autotrader
rm ./build/CMakeCache.txt
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build --config Debug
cp ./build/autotrader ./autotrader

