default: build

build:
  mkdir -p build
  cc -o build/packt src/main.c src/commands.c

clean:
  rm -rf build
