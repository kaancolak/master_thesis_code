//
// Libor Novak
// 03/20/2017
//
// Tests object detection of 2D bounding boxes a single scale detector on an image pyramid. It outputs BBTXT.
//
// Optionaly it can apply non-maxima suppression (merging) on bounding boxes.
//

#include <caffe/caffe.hpp>

// This code only works with OpenCV!
#ifdef USE_OPENCV

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <algorithm>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>
namespace po = boost::program_options;


namespace {

    struct BB2D
    {
        BB2D () {}
        BB2D (const std::string &path_image, int label, double conf, double xmin, double ymin, double xmax,
              double ymax)
            : path_image(path_image),
              label(label),
              conf(conf),
              xmin(xmin),
              ymin(ymin),
              xmax(xmax),
              ymax(ymax)
        {}

        std::string path_image;
        int label;
        double conf;
        double xmin;
        double ymin;
        double xmax;
        double ymax;
    };

}


/**
 * @brief Wraps the input layer into a vector of cv::Mat so we could assign data to it more easily
 * @param input_layer Pointer to the net input layer blob
 * @param input_channels Vector of cv::Mat, which will be assigned
 */
void wrapInputLayer (caffe::Blob<float>* input_layer, std::vector<cv::Mat> &out_input_channels)
{
    out_input_channels.clear();

    int height = input_layer->shape(2);
    int width  = input_layer->shape(3);

    float* input_data = input_layer->mutable_cpu_data();

    for (int i = 0; i < input_layer->shape(1); ++i)
    {
        cv::Mat channel(height, width, CV_32FC1, input_data);
        out_input_channels.push_back(channel);
        input_data += width * height;
    }
}


std::vector<BB2D> extract2DBoundingBoxes (caffe::Blob<float> *output, const std::string &path_image,
                                          double scale)
{
    std::vector<BB2D> bounding_boxes;

    float *data_output = output->mutable_cpu_data();

    // 2D bounding box
    cv::Mat acc_prob(output->shape(2), output->shape(3), CV_32FC1, data_output+output->offset(0, 0));
    cv::Mat acc_xmin(output->shape(2), output->shape(3), CV_32FC1, data_output+output->offset(0, 1));
    cv::Mat acc_ymin(output->shape(2), output->shape(3), CV_32FC1, data_output+output->offset(0, 2));
    cv::Mat acc_xmax(output->shape(2), output->shape(3), CV_32FC1, data_output+output->offset(0, 3));
    cv::Mat acc_ymax(output->shape(2), output->shape(3), CV_32FC1, data_output+output->offset(0, 4));

    // Extract detected boxes
    for (int i = 0; i < acc_prob.rows; ++i)
    {
        for (int j = 0; j < acc_prob.cols; ++j)
        {
            float conf = acc_prob.at<float>(i, j);
            if (conf >= 0.1)
            {
                int xmin = (4*j + (80*(acc_xmin.at<float>(i, j) - 0.5))) / scale;
                int ymin = (4*i + (80*(acc_ymin.at<float>(i, j) - 0.5))) / scale;
                int xmax = (4*j + (80*(acc_xmax.at<float>(i, j) - 0.5))) / scale;
                int ymax = (4*i + (80*(acc_ymax.at<float>(i, j) - 0.5))) / scale;

                bounding_boxes.emplace_back(path_image, 1, conf, xmin, ymin, xmax, ymax);
            }
        }
    }

    return bounding_boxes;
}


std::vector<BB2D> detectObjects (const std::string &path_image, const cv::Mat &image,
                                 const std::vector<double> &scales,
                                 const std::shared_ptr<caffe::Net<float>> &net)
{
    std::vector<BB2D> bounding_boxes;

    caffe::Blob<float>* input_layer  = net->input_blobs()[0];
    caffe::Blob<float>* output_layer = net->output_blobs()[0];

    std::vector<cv::Mat> input_channels;

    // Convert to zero mean and unit variance
    cv::Mat imagef; image.convertTo(imagef, CV_32FC3);
    imagef -= cv::Scalar(128.0f, 128.0f, 128.0f);
    imagef *= 1.0f/128.0f;

    // Build the image pyramid and run detection on each scale of the pyramid
    for (double s: scales)
    {
        cv::Mat imagef_scaled;
        cv::resize(imagef, imagef_scaled, cv::Size(), s, s);

        // Reshape the network
        input_layer->Reshape(1, input_layer->shape(1), imagef_scaled.rows, imagef_scaled.cols);
        net->Reshape();

        // Prepare the cv::Mats for input
        wrapInputLayer(input_layer, input_channels);
        // Copy the image to the input layer of the network
        cv::split(imagef_scaled, input_channels);

        net->Forward();

        std::vector<BB2D> new_bbs = extract2DBoundingBoxes(output_layer, path_image, s);
        bounding_boxes.insert(bounding_boxes.end(), new_bbs.begin(), new_bbs.end());
    }

    return bounding_boxes;
}


void writeBoundingBoxes (const std::vector<BB2D> &bbs, std::ofstream &fout)
{
    for (const BB2D &bb: bbs)
    {
        // The line of a BBTXT file is: filename label confidence xmin ymin xmax ymax
        fout << bb.path_image << " " << bb.label << " " << bb.conf << " " << bb.xmin << " " << bb.ymin
             << " " << bb.xmax << " " << bb.ymax << std::endl;
    }
}


void runPyramidDetection (const std::string &path_prototxt, const std::string &path_caffemodel,
                          const std::string &path_image_list, const std::string &path_out)
{
#ifdef CPU_ONLY
    caffe::Caffe::set_mode(caffe::Caffe::CPU);
#else
    caffe::Caffe::set_mode(caffe::Caffe::GPU);
#endif

    // Scaling factor is 1.5
    // Right now the detectors are trained on object size = 80px. I.e. the scale 1.0 will detect objects
    // around that size
    const std::vector<double> scales = {2.25, 1.5, 1.0, 0.66, 0.44, 0.29, 0.19};

    // Create network and load trained weights from caffemodel file
    auto net = std::make_shared<caffe::Net<float>>(path_prototxt, caffe::TEST);
    net->CopyTrainedLayersFrom(path_caffemodel);

    caffe::Blob<float>* input_layer  = net->input_blobs()[0];
    caffe::Blob<float>* output_layer = net->output_blobs()[0];

    CHECK_EQ(net->num_inputs(), 1) << "Network should have exactly one input.";
    CHECK_EQ(input_layer->shape(1), 3) << "Input layer must have 3 channels.";
    CHECK_EQ(output_layer->shape(1), 5) << "Unsupported network, only 5 channels!";

    std::ifstream infile(path_image_list.c_str());
    CHECK(infile) << "Unable to open image list TXT file '" << path_image_list << "'!";
    std::string line;

    std::ofstream fout; fout.open(path_out);
    CHECK(fout) << "Output file '" << path_out << "' could not have been created!";


    // -- RUN THE DETECTOR ON EACH IMAGE -- //
    while (std::getline(infile, line))
    {
        LOG(INFO) << line;
        CHECK(boost::filesystem::exists(line)) << "Image '" << line << "' not found!";

        // Load the image
        cv::Mat image = cv::imread(line, CV_LOAD_IMAGE_COLOR);

        // Build the image pyramid and run detection on each scale of the pyramid
        std::vector<BB2D> bbs = detectObjects(line, image, scales, net);

        // Save the bounding boxes to a BBTXT file
        writeBoundingBoxes(bbs, fout);
    }

    fout.close();
}



// -----------------------------------------------  MAIN  ------------------------------------------------ //

struct ProgramArguments
{
    std::string path_prototxt;
    std::string path_caffemodel;
    std::string path_image_list;
    std::string path_out;
};


/**
 * @brief Parses arguments of the program
 */
void parseArguments (int argc, char** argv, ProgramArguments &pa)
{
    try {
        po::options_description desc("Arguments");
        desc.add_options()
            ("help", "Print help")
            ("prototxt", po::value<std::string>(&pa.path_prototxt)->required(),
             "Model file of the network (*.prototxt)")
            ("caffemodel", po::value<std::string>(&pa.path_caffemodel)->required(),
             "Weight file of the network (*.caffemodel)")
            ("image_list", po::value<std::string>(&pa.path_image_list)->required(),
             "Path to a TXT file with paths to the images to be tested")
            ("path_out", po::value<std::string>(&pa.path_out)->required(),
             "Path to the output BBTXT file")
        ;

        po::positional_options_description positional;
        positional.add("prototxt", 1);
        positional.add("caffemodel", 1);
        positional.add("image_list", 1);
        positional.add("path_out", 1);


        // Parse the input arguments
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(positional).run(), vm);

        if (vm.count("help")) {
            std::cout << "Usage: ./detect_accumulator path/f.prototxt path/f.caffemodel path/image_list.txt path/out.bbtxt\n";
            std::cout << desc;
            exit(EXIT_SUCCESS);
        }

        po::notify(vm);

        if (!boost::filesystem::exists(pa.path_prototxt))
        {
            std::cerr << "ERROR: File '" << pa.path_prototxt << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
        if (!boost::filesystem::exists(pa.path_caffemodel))
        {
            std::cerr << "ERROR: File '" << pa.path_caffemodel << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
        if (!boost::filesystem::exists(pa.path_image_list))
        {
            std::cerr << "ERROR: File '" << pa.path_image_list << "' does not exist!" << std::endl;
            exit(EXIT_FAILURE);
        }
        if (boost::filesystem::exists(pa.path_out))
        {
            std::cerr << "ERROR: File '" << pa.path_out << "' already exists!" << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    catch(std::exception& e)
    {
        std::cerr << e.what() << "\n";
        exit(EXIT_FAILURE);
    }
}


int main (int argc, char** argv)
{
    FLAGS_logtostderr = 1;
    FLAGS_minloglevel = ::google::INFO;
    ::google::InitGoogleLogging(argv[0]);

    ProgramArguments pa;
    parseArguments(argc, argv, pa);


    runPyramidDetection(pa.path_prototxt, pa.path_caffemodel, pa.path_image_list, pa.path_out);


    return EXIT_SUCCESS;
}


#else
int main(int argc, char** argv) {
    LOG(FATAL) << "This example requires OpenCV; compile with USE_OPENCV.";
}
#endif  // USE_OPENCV
