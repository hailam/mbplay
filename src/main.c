#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#define WIDTH 19200
#define HEIGHT 10800
#define MAX_ITER 1000
#define NUM_THREADS 10

typedef struct
{
    unsigned char r, g, b;
} PixelColor;

// We need a way to tell each thread which part of the image to work on
typedef struct
{
    int start_y;
    int end_y;
    PixelColor *image_data;
} ThreadArgs;

// This is the "Engine" - we moved the math here so threads can run it
void *render_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;

    for (int y = args->start_y; y < args->end_y; y++)
    {
        for (int x = 0; x < WIDTH; x++)
        {
            // map pixel position to a point in the complex plane
            double cx = (x - WIDTH / 2.0) * 3.5 / WIDTH;
            double cy = (y - HEIGHT / 2.0) * 2.0 / HEIGHT;

            double zx = 0.0, zy = 0.0;
            unsigned int iteration = 0;

            // formula: Z = Z^2 + C
            while (zx * zx + zy * zy < 4.0 && iteration < MAX_ITER)
            {
                double xtemp = zx * zx - zy * zy + cx;
                zy = 2.0 * zx * zy + cy;
                zx = xtemp;
                iteration++;
            }

            int pixel_index = y * WIDTH + x;
            if (iteration == MAX_ITER)
            {
                args->image_data[pixel_index] = (PixelColor){0, 0, 0}; // black for points inside the set
            }
            else
            {
                // outdide create colorful
                args->image_data[pixel_index] = (PixelColor){
                    (unsigned char)((iteration * 9) % 256),
                    (unsigned char)((iteration * 7) % 256),
                    (unsigned char)((iteration * 5) % 256)};
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *output_file = "mandelbrot.ppm";
    if (argc > 1)
    {
        output_file = argv[1];
    }

    // lets tell what current directory is
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != 0)
    {
        printf("Current working dir: %s\n", cwd);
    }

    // a giant buffer in RAM
    size_t total_pixels = (size_t)WIDTH * HEIGHT;
    PixelColor *buffer = malloc(total_pixels * sizeof(PixelColor));
    if (!buffer)
    {
        fprintf(stderr, "Could not allocate enough memory for image buffer\n");
        return 1;
    }

    // prep threads
    pthread_t threads[NUM_THREADS];
    ThreadArgs thread_args[NUM_THREADS];
    int rows_per_thread = HEIGHT / NUM_THREADS;

    printf("Computing fractal with %d threads...\n", NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++)
    {
        thread_args[i].start_y = i * rows_per_thread;
        // make sure the last thread goes all the way to the bottom
        thread_args[i].end_y = (i == NUM_THREADS - 1) ? HEIGHT : (i + 1) * rows_per_thread;
        thread_args[i].image_data = buffer;

        pthread_create(&threads[i], NULL, render_thread, &thread_args[i]);
    }

    // let's wait for all threads to finish
    for (int i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    // lets check if the file can be opened
    FILE *fp = fopen(output_file, "wb");
    if (!fp)
    {
        fprintf(stderr, "Error: Could not open file %s for writing.\n", output_file);
        free(buffer);
        return 1;
    }

    // Write the PPM header
    fprintf(fp, "P6\n%d %d\n255\n", WIDTH, HEIGHT);

    // write the whole buffer at once - this is MUCH faster
    printf("Writing to disk...\n");
    fwrite(buffer, sizeof(PixelColor), total_pixels, fp);

    fclose(fp);
    free(buffer);

    printf("Mandelbrot set image saved to %s\n", output_file);
    return 0;
}