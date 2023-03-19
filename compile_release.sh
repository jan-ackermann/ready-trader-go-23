rm ./build/autotrader
rm ./build/CMakeCache.txt
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --config Release
cp ./build/autotrader ./autotrader
