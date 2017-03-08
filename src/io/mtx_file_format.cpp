#include "mtx_file_format.h"

#include <iostream>
#include <sstream>
#include <cstdlib>

#include "tensor_base.h"
#include "util/error.h"

namespace taco {
namespace io {
namespace mtx {

void readFile(std::ifstream &mtxfile,
              int* nrow, int* ncol, int* nnzero,
              TensorBase* tensor) {

  std::string line;
  int rowind,colind;
  double value;
  std::string val;
  while(std::getline(mtxfile,line)) {
    std::stringstream iss(line);
    char firstChar;
    iss >> firstChar;
    // Skip comments
    if (firstChar != '%') {
      iss.clear();
      iss.str(line);
      iss >> *nrow >> *ncol >> *nnzero;
      break;
    }
  }

  while(std::getline(mtxfile,line)) {
    std::stringstream iss(line);
    iss >> rowind >> colind >> val;
    value = std::stod(val);
    tensor->insert({rowind-1,colind-1},value);
  }
  tensor->pack();
}

void writeFile(std::ofstream &mtxfile, std::string name,
               int nrow, int ncol, int nnzero) {
  mtxfile << "%-----------------------------------" << std::endl;
  mtxfile << "% MTX matrix file generated by taco " << std::endl;
  mtxfile << "% name: " << name << std::endl;
  mtxfile << "%-----------------------------------" << std::endl;
  mtxfile << nrow << " " << ncol << " " << " " << nnzero << std::endl;
}


}}}