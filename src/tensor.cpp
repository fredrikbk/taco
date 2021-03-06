#include "taco/tensor.h"

#include <set>
#include <cstring>
#include <fstream>
#include <sstream>
#include <limits.h>

#include "taco/tensor.h"
#include "taco/format.h"
#include "taco/index_notation/index_notation.h"
#include "taco/index_notation/index_notation_nodes.h"
#include "taco/index_notation/index_notation_visitor.h"
#include "taco/storage/storage.h"
#include "taco/storage/index.h"
#include "taco/storage/array.h"
#include "taco/storage/array_util.h"
#include "taco/storage/pack.h"
#include "taco/ir/ir.h"
#include "taco/lower/lower.h"
#include "lower/iteration_graph.h"
#include "codegen/module.h"
#include "taco/taco_tensor_t.h"
#include "taco/storage/file_io_tns.h"
#include "taco/storage/file_io_mtx.h"
#include "taco/storage/file_io_rb.h"
#include "taco/util/strings.h"
#include "taco/util/timers.h"
#include "taco/util/name_generator.h"
#include "taco/error/error_messages.h"
#include "error/error_checks.h"
#include "taco/storage/typed_vector.h"

using namespace std;
using namespace taco::ir;
using namespace taco::storage;

namespace taco {

struct TensorBase::Content {
  string                name;
  vector<int>           dimensions;
  DataType              ctype;

  storage::Storage      storage;

  TensorVar             tensorVar;

  size_t                allocSize;
  size_t                valuesSize;

  Stmt                  assembleFunc;
  Stmt                  computeFunc;
  bool                  assembleWhileCompute;
  shared_ptr<Module>    module;
};

TensorBase::TensorBase() : TensorBase(Float()) {
}

TensorBase::TensorBase(DataType ctype)
    : TensorBase(util::uniqueName('A'), ctype) {
}

TensorBase::TensorBase(std::string name, DataType ctype)
    : TensorBase(name, ctype, {}, Format())  {
}

TensorBase::TensorBase(DataType ctype, vector<int> dimensions, 
                       ModeType modeType)
    : TensorBase(util::uniqueName('A'), ctype, dimensions, 
                 std::vector<ModeTypePack>(dimensions.size(), modeType)) {
}

TensorBase::TensorBase(DataType ctype, vector<int> dimensions, Format format)
    : TensorBase(util::uniqueName('A'), ctype, dimensions, format) {
}

TensorBase::TensorBase(std::string name, DataType ctype, 
                       std::vector<int> dimensions, ModeType modeType)
    : TensorBase(name, ctype, dimensions, 
                 std::vector<ModeTypePack>(dimensions.size(), modeType)) {
}

TensorBase::TensorBase(std::string name, DataType ctype, 
                       std::vector<int> dimensions, Format format) 
    : content(new Content) {
  taco_uassert(format.getOrder() == dimensions.size()) <<
      "The number of format mode types (" << format.getOrder() << ") " <<
      "must match the tensor order (" << dimensions.size() << ").";

  // Initialize coordinate types for Format if not already set
  if (format.getLevelArrayTypes().size() < format.getOrder()) {
    std::vector<std::vector<DataType>> levelArrayTypes;
    for (size_t i = 0; i < format.getOrder(); ++i) {
      std::vector<DataType> arrayTypes;
      ModeType modeType = format.getModeTypes()[i];
      if (modeType == Dense) {
        arrayTypes.push_back(Int32);
      } else if (modeType == Sparse) {
        arrayTypes.push_back(Int32);
        arrayTypes.push_back(Int32);
      } else {
        taco_not_supported_yet;
      }
      levelArrayTypes.push_back(arrayTypes);
    }
    format.setLevelArrayTypes(levelArrayTypes);
  }

  content->name = name;
  content->dimensions = dimensions;
  content->storage = Storage(format);
  content->ctype = ctype;
  content->allocSize = 1 << 20;

  // Initialize dense storage modes
  // TODO: Get rid of this and make code use dimensions instead of dense indices
  vector<ModeIndex> modeIndices(format.getOrder());
  for (size_t i = 0; i < format.getOrder(); ++i) {
    if (format.getModeTypes()[i] == Dense) {
      const size_t idx = format.getModeOrdering()[i];
      modeIndices[i] = ModeIndex({makeArray({dimensions[idx]})});
    }
  }
  content->storage.setIndex(Index(format, modeIndices));

  content->assembleWhileCompute = false;
  content->module = make_shared<Module>();

  std::vector<Dimension> dims;
  long long maxArraySize = 1;
  for (auto& dim : getDimensions()) {
    dims.push_back(dim);
    maxArraySize *= dim;
  }
  
  content->tensorVar = TensorVar(getName(),
                                 Type(getComponentType(), dims),
                                 getFormat());

  this->coordinateBuffer = shared_ptr<vector<char>>(new vector<char>);
  this->coordinateBufferUsed = 0;
  /*
  for (size_t i = Int8.getNumBits(); i <= Int128.getNumBits(); i *= 2) {
    if (maxArraySize <= exp2(i-1) - 1) {
      content->coordinateType = Int(i);
    }
  }*/

  this->coordinateSize = getOrder()*sizeof(int) + ctype.getNumBytes();
}

void TensorBase::setName(std::string name) const {
  content->name = name;
  content->tensorVar.setName(name);
}

string TensorBase::getName() const {
  return content->name;
}

size_t TensorBase::getOrder() const {
  return content->dimensions.size();
}

int TensorBase::getDimension(size_t mode) const {
  taco_uassert(mode < getOrder()) << "Invalid mode";
  return content->dimensions[mode];
}

const vector<int>& TensorBase::getDimensions() const {
  return content->dimensions;
}

const Format& TensorBase::getFormat() const {
  return content->storage.getFormat();
}

void TensorBase::reserve(size_t numCoordinates) {
  size_t newSize = this->coordinateBuffer->size() +
                   numCoordinates*this->coordinateSize;
  this->coordinateBuffer->resize(newSize);
}

const DataType& TensorBase::getComponentType() const {
  return content->ctype;
}

const TensorVar& TensorBase::getTensorVar() const {
  return content->tensorVar;
}

const storage::Storage& TensorBase::getStorage() const {
  return content->storage;
}

storage::Storage& TensorBase::getStorage() {
  return content->storage;
}

void TensorBase::setAllocSize(size_t allocSize) {
  content->allocSize = allocSize;
}

size_t TensorBase::getAllocSize() const {
  return content->allocSize;
}

static size_t numIntegersToCompare = 0;
static int lexicographicalCmp(const void* a, const void* b) {
  for (size_t i = 0; i < numIntegersToCompare; i++) {
    int diff = ((int*)a)[i] - ((int*)b)[i];
    if (diff != 0) {
      return diff;
    }
  }
  return 0;
}

/// Pack coordinates into a data structure given by the tensor format.
void TensorBase::pack() {
  const size_t order = getOrder();
  
  
  // Pack scalars
  if (order == 0) {
    char* coordLoc = this->coordinateBuffer->data();
    void* scalarPtr = &coordLoc[this->coordinateSize - getComponentType().getNumBytes()];
    Array array = makeArray(getComponentType(), 1);
    memcpy(array.getData(), scalarPtr, getComponentType().getNumBytes());
    content->storage.setValues(array);
    this->coordinateBuffer->clear();
    return;
  }
  
  
  /// Permute the coordinates according to the storage mode ordering.
  /// This is a workaround since the current pack code only packs tensors in the
  /// ordering of the modes.
  const std::vector<int>& dimensions = getDimensions();
  taco_iassert(getFormat().getOrder() == order);
  std::vector<size_t> permutation = getFormat().getModeOrdering();
  std::vector<int> permutedDimensions(order);
  for (size_t i = 0; i < order; ++i) {
    permutedDimensions[i] = dimensions[permutation[i]];
  }
  
  taco_iassert((this->coordinateBufferUsed % this->coordinateSize) == 0);
  size_t numCoordinates = this->coordinateBufferUsed / this->coordinateSize;
  const size_t coordSize = this->coordinateSize;
  char* coordinatesPtr = coordinateBuffer->data();
  vector<int> permuteBuffer(order);
  for (size_t i=0; i < numCoordinates; ++i) {
    int* coordinate = (int*)coordinatesPtr;
    for (size_t j = 0; j < order; j++) {
      permuteBuffer[j] = coordinate[permutation[j]];
    }
    for (size_t j = 0; j < order; j++) {
      coordinate[j] = permuteBuffer[j];
    }
    coordinatesPtr += this->coordinateSize;
  }
  coordinatesPtr = coordinateBuffer->data();  
  
  // The pack code expects the coordinates to be sorted
  numIntegersToCompare = order;
  qsort(coordinatesPtr, numCoordinates, coordSize, lexicographicalCmp);
  

  // Move coords into separate arrays and remove duplicates
  std::vector<TypedIndexVector> coordinates(order);
  for (size_t i=0; i < order; ++i) {
    coordinates[i] = TypedIndexVector(getFormat().getCoordinateTypeIdx(i), numCoordinates);
  }
  char* values = (char*) malloc(numCoordinates * getComponentType().getNumBytes());
  // Copy first coordinate-value pair
  int* lastCoord = (int*)malloc(order * sizeof(int));
  int j = 1;
  if (numCoordinates >= 1) {
    int* coordComponent = (int*)coordinatesPtr;
    for (size_t d=0; d < order; ++d) {
      coordinates[d].set(0, *coordComponent);
      lastCoord[d] = *coordComponent;
      coordComponent++;
    }
    memcpy(values, coordComponent, getComponentType().getNumBytes());
  }
  else {
    j = 0;
  }
  // Copy remaining coordinate-value pairs, removing duplicates
  int* coord = (int*)malloc(order * sizeof(int));
  void *value = malloc(getComponentType().getNumBytes());
  for (size_t i=1; i < numCoordinates; ++i) {
    int* coordLoc = (int*)&coordinatesPtr[i*coordSize];
    for (size_t d=0; d < order; ++d) {
      coord[d] = *coordLoc;
      coordLoc++;
    }
    memcpy(value, coordLoc, getComponentType().getNumBytes());
    if (coord != lastCoord) {
      for (size_t d = 0; d < order; d++) {
        coordinates[d].set(j, coord[d]);
      }
      memcpy(&values[j * getComponentType().getNumBytes()], value, getComponentType().getNumBytes());
      j++;
    }
    else {
      taco_uwarning << "Duplicate coordinate ignored when inserting into tensor";
    }
  }
  free(value);
  free(coord);
  free(lastCoord);
  if (numCoordinates > 0) {
    for (size_t i=0; i < order; ++i) {
      coordinates[i].resize(j);
    }
    values = (char *) realloc(values, (j) * getComponentType().getNumBytes());
  }
  taco_iassert(coordinates.size() > 0);
  this->coordinateBuffer->clear();
  this->coordinateBufferUsed = 0;

  // Pack indices and values
  content->storage = storage::pack(permutedDimensions, getFormat(),
                                   coordinates, (void *) values, j, getComponentType());

  free(values);
}

void TensorBase::zero() {
  getStorage().getValues().zero();
}

/// Inherits Access and adds a TensorBase object, so that we can retrieve the
/// tensors that was used in an expression when we later want to pack arguments.
struct AccessTensorNode : public AccessNode {
  AccessTensorNode(TensorBase tensor, const std::vector<IndexVar>& indices)
      :  AccessNode(tensor.getTensorVar(), indices), tensor(tensor) {}
  TensorBase tensor;
  void setAssignment(const Assignment& assignment) {
    tensor.setAssignment(assignment);
  }
};

const Access TensorBase::operator()(const std::vector<IndexVar>& indices) const {
  taco_uassert(indices.size() == getOrder()) <<
      "A tensor of order " << getOrder() << " must be indexed with " <<
      getOrder() << " variables, but is indexed with:  " << util::join(indices);
  return Access(new AccessTensorNode(*this, indices));
}

Access TensorBase::operator()(const std::vector<IndexVar>& indices) {
  taco_uassert(indices.size() == getOrder()) <<
      "A tensor of order " << getOrder() << " must be indexed with " <<
      getOrder() << " variables, but is indexed with:  " << util::join(indices);
  return Access(new AccessTensorNode(*this, indices));
}

void TensorBase::compile(bool assembleWhileCompute) {
  TensorVar tensorVar = getTensorVar();

  taco_uassert(tensorVar.getAssignment().defined())
      << error::compile_without_expr;

  std::set<lower::Property> assembleProperties, computeProperties;
  assembleProperties.insert(lower::Assemble);
  computeProperties.insert(lower::Compute);
  if (assembleWhileCompute) {
    computeProperties.insert(lower::Assemble);
  }

  content->assembleWhileCompute = assembleWhileCompute;
  content->assembleFunc = lower::lower(tensorVar, "assemble",
                                       assembleProperties, getAllocSize());
  content->computeFunc  = lower::lower(tensorVar, "compute",
                                       computeProperties, getAllocSize());
  content->module->addFunction(content->assembleFunc);
  content->module->addFunction(content->computeFunc);
  content->module->compile();
}

/// Pack the tensor's indices and values into a taco_tensor_t object.
static taco_tensor_t* packTensorData(const TensorBase& tensor) {
  taco_tensor_t* tensorData = (taco_tensor_t*)malloc(sizeof(taco_tensor_t));
  size_t order = tensor.getOrder();
  Storage storage = tensor.getStorage();
  Format format = storage.getFormat();

  taco_iassert(order <= INT_MAX);
  tensorData->order         = static_cast<int>(order);
  tensorData->dimensions    = (int32_t*)malloc(order * sizeof(int32_t));
  tensorData->mode_ordering = (int32_t*)malloc(order * sizeof(int32_t));
  tensorData->mode_types    = (taco_mode_t*)malloc(order * sizeof(taco_mode_t));
  tensorData->indices       = (uint8_t***)malloc(order * sizeof(uint8_t***));

  auto index = storage.getIndex();
  for (size_t i = 0; i < tensor.getOrder(); i++) {
    auto modeType  = format.getModeTypes()[i];
    auto modeIndex = index.getModeIndex(i);

    tensorData->dimensions[i] = tensor.getDimension(i);

    size_t m = format.getModeOrdering()[i];
    taco_iassert(m <= INT_MAX);
    tensorData->mode_ordering[i] = static_cast<int>(m);

    if (modeType == Dense) {
      tensorData->mode_types[i] = taco_mode_dense;
      tensorData->indices[i]    = (uint8_t**)malloc(1 * sizeof(uint8_t**));

      const Array& size = modeIndex.getIndexArray(0);
      tensorData->indices[i][0] = (uint8_t*)size.getData();
    } else if (modeType == Sparse) {
      tensorData->mode_types[i]  = taco_mode_sparse;
      tensorData->indices[i]    = (uint8_t**)malloc(2 * sizeof(uint8_t**));

      // When packing results for assemblies they won't have sparse indices
      if (modeIndex.numIndexArrays() == 0) {
        continue;
      }

      const Array& pos = modeIndex.getIndexArray(0);
      const Array& idx = modeIndex.getIndexArray(1);
      tensorData->indices[i][0] = (uint8_t*)pos.getData();
      tensorData->indices[i][1] = (uint8_t*)idx.getData();
    } else {
      taco_not_supported_yet;
    }
  }

  taco_iassert(tensor.getComponentType().getNumBits() <= INT_MAX);
  tensorData->csize = static_cast<int>(tensor.getComponentType().getNumBits());
  tensorData->vals  = (uint8_t*)storage.getValues().getData();

  return tensorData;
}

static void freeTensorData(taco_tensor_t* tensorData) {
  for (int i = 0; i < tensorData->order; i++) {
    free(tensorData->indices[i]);
  }
  free(tensorData->indices);

  free(tensorData->dimensions);
  free(tensorData->mode_ordering);
  free(tensorData->mode_types);
  free(tensorData);
}

taco_tensor_t* TensorBase::getTacoTensorT() {
  return packTensorData(*this);
}

static size_t unpackTensorData(const taco_tensor_t& tensorData,
                               const TensorBase& tensor) {
  auto storage = tensor.getStorage();
  auto format = storage.getFormat();

  vector<ModeIndex> modeIndices;
  size_t numVals = 1;
  for (size_t i = 0; i < tensor.getOrder(); i++) {
    ModeType modeType = format.getModeTypes()[i];
    if (modeType == Dense) {
      Array size = makeArray({*(int*)tensorData.indices[i][0]});
      modeIndices.push_back(ModeIndex({size}));
      numVals *= ((int*)tensorData.indices[i][0])[0];
    } else if (modeType == Sparse) {
      auto size = ((int*)tensorData.indices[i][0])[numVals];
      Array pos = Array(type<int>(), tensorData.indices[i][0], numVals+1);
      Array idx = Array(type<int>(), tensorData.indices[i][1], size);
      modeIndices.push_back(ModeIndex({pos, idx}));
      numVals = size;
    } else {
      taco_not_supported_yet;
    }
  }
  storage.setIndex(Index(format, modeIndices));
  storage.setValues(Array(tensor.getComponentType(), tensorData.vals, numVals));
  return numVals;
}

static inline vector<TensorBase> getTensors(const IndexExpr& expr) {
  struct GetOperands : public IndexNotationVisitor {
    using IndexNotationVisitor::visit;
    set<TensorBase> inserted;
    vector<TensorBase> operands;
    void visit(const AccessNode* node) {
      taco_iassert(isa<AccessTensorNode>(node)) << "Unknown subexpression";
      TensorBase tensor = to<AccessTensorNode>(node)->tensor;
      if (!util::contains(inserted, tensor)) {
        inserted.insert(tensor);
        operands.push_back(tensor);
      }
    }
  };
  GetOperands getOperands;
  expr.accept(&getOperands);
  return getOperands.operands;
}

static inline
vector<void*> packArguments(const TensorBase& tensor) {
  vector<void*> arguments;

  // Pack the result tensor
  arguments.push_back(packTensorData(tensor));

  // Pack operand tensors
  auto operands = getTensors(tensor.getTensorVar().getAssignment().getRhs());
  for (auto& operand : operands) {
    arguments.push_back(packTensorData(operand));
  }

  return arguments;
}

void TensorBase::assemble() {
  taco_uassert(this->content->assembleFunc.defined())
      << error::assemble_without_compile;

  auto arguments = packArguments(*this);
  content->module->callFuncPacked("assemble", arguments.data());

  if (!content->assembleWhileCompute) {
    taco_tensor_t* tensorData = ((taco_tensor_t*)arguments[0]);
    content->valuesSize = unpackTensorData(*tensorData, *this);
  }
  for (auto& argument : arguments) freeTensorData((taco_tensor_t*)argument);
}

void TensorBase::compute() {
  taco_uassert(this->content->computeFunc.defined())
      << error::compute_without_compile;

  auto arguments = packArguments(*this);
  this->content->module->callFuncPacked("compute", arguments.data());

  if (content->assembleWhileCompute) {
    taco_tensor_t* tensorData = ((taco_tensor_t*)arguments[0]);
    content->valuesSize = unpackTensorData(*tensorData, *this);
  }
  for (auto& argument : arguments) freeTensorData((taco_tensor_t*)argument);
}

void TensorBase::evaluate() {
  this->compile();
  if (!getTensorVar().getAssignment().getOperator().defined()) {
    this->assemble();
  }
  this->compute();
}

void TensorBase::operator=(const IndexExpr& expr) {
  taco_uassert(getOrder() == 0)
      << "Must use index variable on the left-hand-side when assigning an "
      << "expression to a non-scalar tensor.";
  setAssignment(Assignment(getTensorVar(), {}, expr));
}

void TensorBase::setAssignment(Assignment assignment) {
  content->tensorVar.setAssignment(makeReductionNotation(assignment));
}

void TensorBase::printComputeIR(ostream& os, bool color, bool simplify) const {
  IRPrinter printer(os, color, simplify);
  printer.print(content->computeFunc.as<Function>()->body);
}

void TensorBase::printAssembleIR(ostream& os, bool color, bool simplify) const {
  IRPrinter printer(os, color, simplify);
  printer.print(content->assembleFunc.as<Function>()->body);
}

string TensorBase::getSource() const {
  return content->module->getSource();
}

void TensorBase::compileSource(std::string source) {
  taco_iassert(getTensorVar().getAssignment().getRhs().defined())
      << error::compile_without_expr;

  set<lower::Property> assembleProperties, computeProperties;
  assembleProperties.insert(lower::Assemble);
  computeProperties.insert(lower::Compute);

  TensorVar tensorVar = getTensorVar();
  content->assembleFunc = lower::lower(tensorVar, "assemble",
                                       assembleProperties, getAllocSize());
  content->computeFunc  = lower::lower(tensorVar, "compute",
                                       computeProperties, getAllocSize());

  stringstream ss;
  CodeGen_C::generateShim(content->assembleFunc, ss);
  ss << endl;
  CodeGen_C::generateShim(content->computeFunc, ss);
  content->module->setSource(source + "\n" + ss.str());
  content->module->compile();
}

template<typename T>
bool scalarEquals(T a, T b) {
  double diff = ((double) a - (double) b)/(double)a;
  if (abs(diff) > 10e-6) {
    return false;
  }
  return true;
}

template<typename T>
bool scalarEquals(std::complex<T> a, std::complex<T> b) {
  T diff = std::abs((a - b)/a);
  if ((diff > 10e-6) || (diff < -10e-6)) {
    return false;
  }
  return true;
}
  
template<typename T>
bool equalsTyped(const TensorBase& a, const TensorBase& b) {
  auto at = iterate<T>(a);
  auto bt = iterate<T>(b);
  auto ait = at.begin();
  auto bit = bt.begin();
  
  for (; ait != at.end() && bit != bt.end(); ++ait, ++bit) {
    if (ait->first != bit->first) {
      return false;
    }
    if (!scalarEquals(ait->second, bit->second)) {
      return false;
    }
  }
  return (ait == at.end() && bit == bt.end());
}  

bool equals(const TensorBase& a, const TensorBase& b) {
  // Component type must be the same
  if (a.getComponentType() != b.getComponentType()) {
    return false;
  }

  // Orders must be the same
  if (a.getOrder() != b.getOrder()) {
    return false;
  }

  // Dimensions must be the same
  for (size_t mode = 0; mode < a.getOrder(); mode++) {
    if (a.getDimension(mode) != b.getDimension(mode)) {
      return false;
    }
  }

  // Values must be the same
  switch(a.getComponentType().getKind()) {
    case DataType::Bool: taco_ierror; return false;
    case DataType::UInt8: return equalsTyped<uint8_t>(a, b);
    case DataType::UInt16: return equalsTyped<uint16_t>(a, b);
    case DataType::UInt32: return equalsTyped<uint32_t>(a, b);
    case DataType::UInt64: return equalsTyped<uint64_t>(a, b);
    case DataType::UInt128: return equalsTyped<unsigned long long>(a, b);
    case DataType::Int8: return equalsTyped<int8_t>(a, b);
    case DataType::Int16: return equalsTyped<int16_t>(a, b);
    case DataType::Int32: return equalsTyped<int32_t>(a, b);
    case DataType::Int64: return equalsTyped<int64_t>(a, b);
    case DataType::Int128: return equalsTyped<long long>(a, b);
    case DataType::Float32: return equalsTyped<float>(a, b);
    case DataType::Float64: return equalsTyped<double>(a, b);
    case DataType::Complex64: return equalsTyped<std::complex<float>>(a, b);
    case DataType::Complex128: return equalsTyped<std::complex<double>>(a, b);
    case DataType::Undefined: taco_ierror; return false;
  }

  return false;
}

bool operator==(const TensorBase& a, const TensorBase& b) {
  return a.content == b.content;
}

bool operator!=(const TensorBase& a, const TensorBase& b) {
  return a.content != b.content;
}

bool operator<(const TensorBase& a, const TensorBase& b) {
  return a.content < b.content;
}

bool operator>(const TensorBase& a, const TensorBase& b) {
  return a.content > b.content;
}

bool operator<=(const TensorBase& a, const TensorBase& b) {
  return a.content <= b.content;
}

bool operator>=(const TensorBase& a, const TensorBase& b) {
  return a.content >= b.content;
}

ostream& operator<<(ostream& os, const TensorBase& tensor) {
  vector<string> dimensionStrings;
  for (int dimension : tensor.getDimensions()) {
    dimensionStrings.push_back(to_string(dimension));
  }
  os << tensor.getName() << " (" << util::join(dimensionStrings, "x") << ") "
     << tensor.getFormat() << ":" << std::endl;

  // Print coordinates
  size_t numCoordinates = tensor.coordinateBufferUsed / tensor.coordinateSize;
  for (size_t i = 0; i < numCoordinates; i++) {
    int* ptr = (int*)&tensor.coordinateBuffer->data()[i*tensor.coordinateSize];
    os << "(" << util::join(ptr, ptr+tensor.getOrder()) << "): ";
    switch(tensor.getComponentType().getKind()) {
      case DataType::Bool: taco_ierror; break;
      case DataType::UInt8: os << ((uint8_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::UInt16: os << ((uint16_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::UInt32: os << ((uint32_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::UInt64: os << ((uint64_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::UInt128: os << ((unsigned long long*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Int8: os << ((int8_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Int16: os << ((int16_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Int32: os << ((int32_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Int64: os << ((int64_t*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Int128: os << ((long long*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Float32: os << ((float*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Float64: os << ((double*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Complex64: os << ((std::complex<float>*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Complex128: os << ((std::complex<double>*)(ptr+tensor.getOrder()))[0] << std::endl; break;
      case DataType::Undefined: taco_ierror; break;
    }
  }

  // Print packed data
  os << tensor.getStorage();

  return os;
}

static string getExtension(string filename) {
  return filename.substr(filename.find_last_of(".") + 1);
}

template <typename T, typename U>
TensorBase dispatchRead(T& file, FileType filetype, U format, bool pack) {
  TensorBase tensor;
  switch (filetype) {
    case FileType::ttx:
    case FileType::mtx:
      tensor = readMTX(file, format, pack);
      break;
    case FileType::tns:
      tensor = readTNS(file, format, pack);
      break;
    case FileType::rb:
      tensor = readRB(file, format, pack);
      break;
  }
  return tensor;
}

template <typename U>
TensorBase dispatchRead(std::string filename, U format, bool pack) {
  string extension = getExtension(filename);

  TensorBase tensor;
  if (extension == "ttx") {
    tensor = dispatchRead(filename, FileType::ttx, format, pack);
  }
  else if (extension == "tns") {
    tensor = dispatchRead(filename, FileType::tns, format, pack);
  }
  else if (extension == "mtx") {
    tensor = dispatchRead(filename, FileType::mtx, format, pack);
  }
  else if (extension == "rb") {
    tensor = dispatchRead(filename, FileType::rb, format, pack);
  }
  else {
    taco_uerror << "File extension not recognized: " << filename << std::endl;
  }

  string name = filename.substr(filename.find_last_of("/") + 1);
  name = name.substr(0, name.find_first_of("."));
  std::replace(name.begin(), name.end(), '-', '_');
  tensor.setName(name);

  return tensor;
}

TensorBase read(std::string filename, ModeType modetype, bool pack) {
  return dispatchRead(filename, modetype, pack);
}

TensorBase read(std::string filename, Format format, bool pack) {
  return dispatchRead(filename, format, pack);
}

TensorBase read(string filename, FileType filetype, ModeType modetype, 
                bool pack) {
  return dispatchRead(filename, filetype, modetype, pack);
}

TensorBase read(string filename, FileType filetype, Format format, bool pack) {
  return dispatchRead(filename, filetype, format, pack);
}

TensorBase read(istream& stream, FileType filetype, ModeType modetype, 
                bool pack) {
  return dispatchRead(stream, filetype, modetype, pack);
}

TensorBase read(istream& stream, FileType filetype, Format format, bool pack) {
  return dispatchRead(stream, filetype, format, pack);
}

template <typename T>
void dispatchWrite(T& file, const TensorBase& tensor, FileType filetype) {
  switch (filetype) {
    case FileType::ttx:
    case FileType::mtx:
      writeMTX(file, tensor);
      break;
    case FileType::tns:
      writeTNS(file, tensor);
      break;
    case FileType::rb:
      writeRB(file, tensor);
      break;
  }
}

void write(string filename, const TensorBase& tensor) {
  string extension = getExtension(filename);
  if (extension == "ttx") {
    dispatchWrite(filename, tensor, FileType::ttx);
  }
  else if (extension == "tns") {
    dispatchWrite(filename, tensor, FileType::tns);
  }
  else if (extension == "mtx") {
    taco_iassert(tensor.getOrder() == 2) <<
       "The .mtx format only supports matrices. Consider using the .ttx format "
       "instead";
    dispatchWrite(filename, tensor, FileType::mtx);
  }
  else if (extension == "rb") {
    dispatchWrite(filename, tensor, FileType::rb);
  }
  else {
    taco_uerror << "File extension not recognized: " << filename << std::endl;
  }
}

void write(string filename, FileType filetype, const TensorBase& tensor) {
  dispatchWrite(filename, tensor, filetype);
}

void write(ofstream& stream, FileType filetype, const TensorBase& tensor) {
  dispatchWrite(stream, tensor, filetype);
}

void packOperands(const TensorBase& tensor) {
  auto operands = getTensors(tensor.getTensorVar().getAssignment().getRhs());
  for (TensorBase operand : operands) {
    operand.pack();
  }
}

}
