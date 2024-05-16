CC=clang++
CXXFLAGS=-std=c++20
SRC=source
TEMP=temp

SRCS=$(wildcard $(SRC)/*.cc)
OBJS=$(SRCS:$(SRC)/%.cc=%)

all: rebs

rebs: $(OBJS)
	$(CC) -o rebs $(OBJS:%=$(TEMP)/%)

%: $(SRC)/%.cc
	@mkdir -p $(TEMP)
	$(CC) $(CXXFLAGS) -c -o $(TEMP)/$@ $< -I third_party/

clean:
	$(RM) $(TEMP)/*