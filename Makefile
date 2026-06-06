main.exe: main.cpp
	cl.exe main.cpp /EHsc /std:c++17 /Fe:main.exe /link user32.lib

clean:
	del main.exe main.obj
