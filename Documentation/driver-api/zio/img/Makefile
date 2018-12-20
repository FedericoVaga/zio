
# pdftex picks png before pdf, and the result is crap.
# so let's use gif for the html version, instead of png (/me disgusted)

IN := $(wildcard *.ps *.eps)

PDF := $(patsubst %.ps,%.pdf,$(IN))
PDF := $(patsubst %.eps,%.pdf,$(PDF))

GIF := $(patsubst %.pdf,%.gif,$(PDF))

all: $(PDF) $(GIF)

%.pdf: %.ps
	epstopdf $^

%.pdf: %.eps
	epstopdf $^

%.gif: %.pdf
	convert $^ $@
