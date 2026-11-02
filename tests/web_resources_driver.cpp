#include "web/web_resources.hpp"
#include <iostream>
int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) return 1;
  try {
    const auto root = reaweb::fs::u8path(argv[1]);
    const auto profile = reaweb::fs::u8path(argv[2]);
    const auto entry = reaweb::fs::u8path(argc == 5 ? argv[4] : "index.html");
    auto id = reaweb::app_identity(root);
    reaweb::AppBinding binding;
    if (id.empty()) {
      binding = reaweb::open_app_binding(root, profile, entry);
      id = binding.id;
    } else binding = {id, reaweb::bind_app_identity(root, profile, id, entry)};
    reaweb::WebResources resources(root, id);
    std::cout << resources.origin() << std::endl;
    std::string line;
    while (std::getline(std::cin, line) && !line.empty()) {
      const auto input = reaweb::Json::parse(line);
      const auto response = resources.request(input.at("uri"), input.value("method", "GET"),
        input.value("headers", std::map<std::string, std::string>{}));
      std::cout << reaweb::Json{{"status", response.status}, {"headers", response.headers},
        {"body", reaweb::encode_binary(response.data, response.size)}}.dump() << std::endl;
    }
  } catch (const reaweb::Error& e) {
    std::cout << e.code << ": " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) { std::cout << e.what() << std::endl; return 3; }
}
