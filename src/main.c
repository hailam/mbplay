#include <stdio.h>
#include <unistd.h>

#define WIDTH 19200
#define HEIGHT 10800
#define MAX_ITER 10000

typedef struct
{
    unsigned char r, g, b;
} PixelColor;

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
    else
    {
        perror("getcwd() error");
    }

    // lets check if the file can be opened
    FILE *fp = fopen(output_file, "wb");
    if (!fp)
    {
        fprintf(stderr, "Error: Could not open file %s for writing.\n", output_file);
        return 1;
    }

    // Write the PPM header
    fprintf(fp, "P6\n%d %d\n255\n", WIDTH, HEIGHT);

    // first loop over the image height
    for (int y = 0; y < HEIGHT; y++)
    {
        // then loop over the image width
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

            PixelColor color;
            if (iteration == MAX_ITER)
            {
                color = (PixelColor){0, 0, 0}; // black for points inside the set
            }
            else
            {
                // outdide create colorful
                color = (PixelColor){
                    (unsigned char)((iteration * 9) % 256),
                    (unsigned char)((iteration * 7) % 256),
                    (unsigned char)((iteration * 5) % 256)};
            }

            // write the pixel color to the file
            fwrite(&color, sizeof(PixelColor), 1, fp);
        }
    }
    fclose(fp);

    printf("Mandelbrot set image saved to %s\n", output_file);
    return 0;
}
