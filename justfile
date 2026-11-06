default: build

build:
  mkdir -p build
  cc -o build/packt src/main.c

clean:
  rm -rf build
