/**
 * open an ip socket on the given port
 * accept input from mod_ml apache processes
 * the input should be in the form of 
 * {apache host} {ip} 
 * or 
 * {apache host}/{ip}
 * the host/ip pair are looked up in the botstats table
 * if stats info for that host/ip is found then it 
 * is fed to each of the arbitrary number of serialized weka models
 * provided and the most voted for class is then returned as 
 * YES if the ip is believed to be a bot, NO if it is not or 
 * NOTFOUND if there is no record of it
 *
 * a mod_ml preprocessor (using the MLPreprocessor directive) is
 * needed to update the botstats table - see botlogger.py and botlog.py 
 *
 * references:
 * see https://weka.wikispaces.com/Creating+an+ARFF+file
 * also http://crunchify.com/how-to-read-json-object-from-file-in-java/
 */

import java.io.ObjectInputStream;
import java.io.FileInputStream;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.DataOutputStream;
import java.io.DataInputStream;
import java.io.PrintWriter;
import java.io.IOException;

import weka.classifiers.Classifier;

/**
 */
public class BotPrintClassifier {

    public static boolean verbose = false;

    /**
     * grab a model file and deserialize it into a model
     * see https://weka.wikispaces.com/Serialization
     */
    public static void unpack(String path) {
        try {
            ObjectInputStream ois = new ObjectInputStream(
                    new FileInputStream(path));
            Classifier c = (Classifier) ois.readObject();
            System.out.println(c);

        } catch (Exception e) {
            e.printStackTrace();
            System.exit(1);
        }
    }

    /**
     * read the http_host and remote_addr from the command line
     * look up relevant stats data for my models
     * make weka compatible instance data for this if it exists
     * make a prediction
     * return the result
     */
    public static void main(String[] args) throws Exception {
        for (int i=0; i<args.length; i++) {
            unpack(args[i]);
        }
    }
}

