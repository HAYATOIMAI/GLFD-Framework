#include "Engine.h"
#include "Core/GameConfig.h"
#include "Core/HashMap.h"
#include <iostream>

#ifdef  _WIN32
#include <windows.h>
#endif //  _WIN32

int main() {
 
  GLFD::GameEngine engine;
  
  engine.Initialize();
  
  engine.Run();

  return 0;
}