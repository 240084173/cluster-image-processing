#include "deconvfilter.h"
#include "debug.h"
#include <cstdio>
#include <cstring>
#include <omp.h>

#define PARALLEL
#define EPS 0.000001


#ifndef USE_FFT // small kernel size so use manual convolution

/**
 * Normalises a matrix so that all elements sum to 1.
 */
void normalise(double* data, size_t length) {
    unsigned int i;
    double sum = 0;
    for (i = 0; i < length; i++)
        sum += data[i];
    for (i = 0; i < length; i++)
        data[i] /= sum;
}

/**
 * Allocates scratch space for the algorithm and stores parameters.
 * Width and height are fixed for future images, as is the psf.
 */
DeconvFilter::DeconvFilter(
        int width, int height, unsigned int niter,
        double* inputPsf, int psfWidth, int psfHeight, double* buffer) :
            _width(width),
            _height(height),
            _psfWidth(psfWidth),
            _psfHeight(psfHeight),
            _niter(niter),
            _size(width*height),
            _buffer(buffer),
            orig(buffer) {
    // Save the PSF
    size_t psfSize = psfWidth * psfHeight;
    _psf = new double[psfSize];
    memcpy(_psf, inputPsf, psfSize*sizeof(*_psf));
    psfStartOffset = psfSize / 2;
    normalise(_psf, psfSize);

    // Allocate space
    img      =new double[_size];
    scratch  =new double[_size];
    scratch2 =new double[_size];
}

// Frees space before destruction.
DeconvFilter::~DeconvFilter() {
    delete[] img;
    delete[] scratch;
    delete[] scratch2;
}


/**
 * A convolution algorith optimised for small kernels
 * result = input * _psf
 */
void DeconvFilter::convolve(double* result, double* input) {
    int x, y, index;
    int px, py, pIndex;
    int pxOffset = _psfWidth/2;
    int pyOffset = _psfHeight/2;
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (index = 0; index < (unsigned int)_size; index++)
        result[index] = 0;

#ifdef PARALLEL
#pragma omp parallel for firstprivate(pxOffset,pyOffset) private(index,x,y,px,py,pIndex) shared(result,input) default(none)
#endif
    for (y = 0; y < 1024; y++) { // loop over rows (seperately by thread)
        for (x = 0; x < 1024; x++) { // loop over columns
            pIndex = 0;
            index = x+y*_width; // gcc optimises to this anyway if this is placed deeper
            for (py = 0; py < _psfHeight; py++) { // Iterate through the psf
                for (px = 0; px < _psfWidth; px++,pIndex++) {
                    if (_psf[pIndex] == 0.0) continue;
                    int absY = y - pyOffset + py;
                    if (
                            absY < 0 &&
                            absY >= _height
                       ) break; // whole  psf row is pointless

                    int absX = x - pxOffset + px;
                    if (
                            absX >= 0 ||
                            absX < _width
                       ) {
                        result[index] += input[absX + _width*absY] * _psf[pIndex];
                    }
                }
            }
        }
    }
}

/**
 * Performs n iterations of the Richardson-Lucy deconvolution algorithm
 * without using fast fourier transform, useful when the psf kernel size is
 * small.
 */
void DeconvFilter::process() {
    unsigned int iter;
    uint32_t index;
    for (index = 0; index < _size; index++) {
        img[index] = _buffer[index];
        orig[index] = _buffer[index];
    }

    PerfTimer imageTimer;
    imageTimer.begin();
    for (iter = 0; iter < _niter; iter++) {
        convolve(scratch, img);
        divide(scratch, orig, scratch);
        convolve(scratch2, scratch);
        multiply(img, img, scratch2);
        saturate(img);
    }
    FPRINT("Finished %d iterations on an image in %f seconds", _niter, imageTimer.getElapsed());

    for (index = 0; index < _size; index++)
        _buffer[index] = img[index];
}


void DeconvFilter::saturate(double *image) {
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (uint32_t i = 0; i < _size; i++)
        if (image[i] > 255) image[i] = 255;
        else if (image[i] < 0) image[i] = 0;
}

// quotient[i] = dividend[i] / divisor[i] forall i
void DeconvFilter::divide(double* quotient, double* dividend, double* divisor) {
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (unsigned int i = 0; i < _size; i++)
        quotient[i] = dividend[i] / divisor[i];
}

// product[i] = factorA[i] * factorB[i] forall i
void DeconvFilter::multiply(double* product, double* factorA, double* factorB) {
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (unsigned int i = 0; i < _size; i++)
        product[i] = factorA[i] * factorB[i];
}

// -------- Helper functions (unused) --------- //

// product[i] = product[i] * scalar forall i
void DeconvFilter::scale(double* product, double scalar) {
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (unsigned int i = 0; i < _size; i++)
        product[i] *= scalar;
}

// product[i] = product[i] + offset forall i
void DeconvFilter::offset(double* product, double amount) {
#ifdef PARALLEL
#pragma omp parallel for
#endif
    for (unsigned int i = 0; i < _size; i++)
        product[i] += amount;
}

void minMax(double* buffer, int size) {
    double min = 1e9, max = -1e9;
    for (int i = 0; i < size; i++) {
        if (buffer[i] > max) max = buffer[i];
        if (buffer[i] < min) min = buffer[i];
    }
    FPRINT("Min val = %e, Max val = %e", min, max);
}




// ------- Same as above except with FFTs, for use with larger kernel------ //

#else // if we have a relatively large kernel then use the A*B=ifft(fft(A).*fft(B)) identity

/**
 * Returns an object ready to start filtering
 * images of the given size with the input psf.
 * All scratch space is preallocated for efficiency
 *
 * Will attempt to load FFTW wisdom from file,
 * and will create it otherwise causing the
 * first run to be slow.
 */
DeconvFilter::DeconvFilter(
        int width, int height, unsigned int niter,
        double* inputPsf, int psfWidth, int psfHeight, double* buffer) :
_width(width), _height(height), _niter(niter), _buffer(buffer){
    // Initiate the multithreaded fftw lib. The second line causes
    // a segfault during planning unfortunately so plans are single
    // threaded for now.
    //fftw_init_threads();
    //fftw_plan_with_nthreads(2);//omp_get_num_threads());

    // Allocate space
    _size = width*height;
    in      = fftw_alloc_complex(_size);
    scratch = fftw_alloc_complex(_size);
    orig    = fftw_alloc_complex(_size);
    fftPsf     = fftw_alloc_complex(_size);
    conjFftPsf = fftw_alloc_complex(_size);

    // Load wisdom from file, exploring options is very time consuming
    // so this saves a lot of effort.
    int imported = fftw_import_wisdom_from_filename("wisdom");
    const char* msg = imported ? "Wisdom imported" : "Wisdom not found";
    PRINT(msg);


    // Create fft plans
    fftScratch = fftw_plan_dft_2d(
            width, height, scratch, scratch, FFTW_FORWARD, FFTW_PATIENT);
    ifftScratch = fftw_plan_dft_2d(
            width, height, scratch, scratch, FFTW_BACKWARD, FFTW_ESTIMATE);
    fftInToScratch = fftw_plan_dft_2d(
            width, height, in, scratch, FFTW_FORWARD, FFTW_ESTIMATE);

    centrePsf(fftPsf, inputPsf, width, height, psfWidth, psfHeight);
    fftw_plan fft = fftw_plan_dft_2d(
            width, height, fftPsf, fftPsf, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_execute(fft);

    // If we didn't load from a file, then save to one for the next guy
    if (!imported) fftw_export_wisdom_to_filename("wisdom");

    // Save the PSF's complex conjugate
    for (uint32_t i = 0; i < _size; i++) {
        conjFftPsf[i][0] = fftPsf[i][0];
        conjFftPsf[i][1] = -fftPsf[i][0];
    }
}

/**
 * Destroy all allocated memory
 */
DeconvFilter::~DeconvFilter() {
    fftw_free(in);
    fftw_free(scratch);
    fftw_free(fftPsf);
    fftw_free(conjFftPsf);
    fftw_destroy_plan(fftScratch);
    fftw_destroy_plan(ifftScratch);
    fftw_destroy_plan(fftInToScratch);
    fftw_cleanup_threads();
}

/**
 * Top level of the FFT-based LR algorithm
 */
void DeconvFilter::process() {
    fftw_complex *temp;
    unsigned int i, iter;
    double* scale = NULL;
    for (i = 0; i < _size ; i++) {
        orig[i][0] = in[i][0] = _buffer[i];
        orig[i][1] = in[i][1] = 0.0;
    }
    for (iter = 0; iter < _niter; iter++) {
        fftw_execute(fftInToScratch);
        multVec(scratch, fftPsf, scratch, _size, &scale);
        fftw_execute(ifftScratch);
        divVec(scratch,orig,scratch,_size);
        fftw_execute(fftScratch);
        multVec(scratch, scratch, conjfftPsf, _size, &scale);
        fftw_execute(ifftScratch);
        multVec(in, in, scratch, _size, &scale);
    }
    for (i = 0; i < _size ; i++) {
        _buffer[i] = in[i][0];
    }
}

/**
 * lval = a * b in complex space when n > 0
 * lval = a * b in real    space when n < 0
 * if scale is not null then the results are multiplied by *scale
 */
void DeconvFilter::multVec(fftw_complex* lval, fftw_complex* a, fftw_complex* b, int n, double *scale) {
    double tmp;
    if (--n>=0) {
        if (scale) {
            do {
                tmp = a[n][0]*b[n][0] - a[n][1]*b[n][1];
                lval[n][1] = a[n][0]*b[n][1] + a[n][1]*b[n][0];
                lval[n][0] = tmp;
            } while (--n > -1);
        } else {
            double s = *scale;
            do {
                tmp = (a[n][0]*b[n][0] - a[n][1]*b[n][1]) * s;
                lval[n][1] = (a[n][0]*b[n][1] + a[n][1]*b[n][0]) * s;
                lval[n][0] = tmp;
            } while (--n > -1);
        }
    } else {
        n = n*-1 - 1;
        if (scale) {
            do
                lval[n][1] = a[n][0]*b[n][0];
            while (--n > -1);
        } else {
            double s = *scale;
            do
                lval[n][0] = (a[n][0]*b[n][0]) * s;
            while (--n > -1);
        }
    }
}

/**
 * lval.re = a.re / b.re
 * lval.im = a.im / b.im
 * On an a per element basis.
 */
void DeconvFilter::divVec(fftw_complex* lval, fftw_complex* a, fftw_complex* b, int n) {
    for (int i = 0; i < n; i++) {
        lval[i][0] = a[i][0]/(b[i][0]+EPS);
        lval[i][1] = a[i][1]/(b[i][1]+EPS);
    }
}

/**
 * Loads a psf into mat centred at the top left corner with the
 * rest of mat zeroed out.
 */
void DeconvFilter::centrePsf(
        fftw_complex* mat, double* input, int width, int height,
        int psfWidth, int psfHeight) {
    int i, j, index = 0;
    for (i = 0; i < width; i++) {
        for (j = 0; j < height; j++, index++) {
            mat[index][0] = 0.0;
            mat[index][1] = 0.0;
        }
    }

    index = 0;
    for (i = psfWidth/2; i < psfWidth; i++) {
        for (j = psfHeight/2; j < psfHeight; j++, index++) {
            mat[index][0] = input[i*psfWidth+j];
        }
        index += width - psfWidth/2;
    }
}

#endif



