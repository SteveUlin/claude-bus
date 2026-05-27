#include <iostream>
#include <fstream>
#include <string>

int main() {
    std::ofstream out("test.txt");
    out << "line 1\npart";
    out.close();

    std::ifstream in("test.txt");
    std::string line;
    auto pos = in.tellg();
    while (std::getline(in, line)) {
        std::cout << "Read: " << line << ", eof: " << in.eof() << ", fail: " << in.fail() << "\n";
        if (in.eof()) {
            std::cout << "Partial line! Seeking back to " << pos << "\n";
            in.clear();
            in.seekg(pos);
            break;
        }
        pos = in.tellg();
    }
    
    std::ofstream out2("test.txt", std::ios::app);
    out2 << "ial line\n";
    out2.close();

    while (std::getline(in, line)) {
        std::cout << "Read next: " << line << "\n";
    }
    return 0;
}
