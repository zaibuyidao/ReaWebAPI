#include "web/web_resources.hpp"
#include <iostream>
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  try {
    reaweb::WebResources resources(reaweb::fs::u8path(argv[1]), reaweb::fs::u8path(argv[2]));
    std::cout << resources.origin() << std::endl;
    std::string line;
    std::getline(std::cin, line);
  } catch (const reaweb::Error& e) {
    std::cout << e.code << ": " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) { std::cout << e.what() << std::endl; return 3; }
}
