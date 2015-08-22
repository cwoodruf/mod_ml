// see https://weka.wikispaces.com/Creating+an+ARFF+file
// also http://crunchify.com/how-to-read-json-object-from-file-in-java/
import org.json.simple.JSONObject;
import org.json.simple.parser.*;
import java.util.*;
import org.apache.commons.lang.ArrayUtils;

import weka.core.Attribute;
import weka.core.FastVector;
import weka.core.Instance;
import weka.core.Instances;

/**
 * Generates a little ARFF file with different attribute types.
 *
 * @author FracPete
 */
public class CLIClassifier {
    public static void main(String[] args) throws Exception {
        // for the weka stuff
        FastVector        atts;
        FastVector        labels;
        Instances         data;
        Iterator<String>  keys;
        ArrayList         values;
        int               i;

        JSONParser parser = new JSONParser();
        // see https://code.google.com/p/json-simple/wiki/DecodingExamples
        ContainerFactory containerFactory = new ContainerFactory(){
            public List creatArrayContainer() {
                return new LinkedList();
            }
            public Map createObjectContainer() {
                return new LinkedHashMap();
            }
        };

        // 1. set up attributes
        atts = new FastVector();

        // parse an input object
        try {
            System.out.println("parsing: "+args[0]);
            Map json = (Map) parser.parse(args[0]);
            Iterator iter = json.entrySet().iterator();
            values = new ArrayList<Double>();

            for (i=0; iter.hasNext(); i++) {
                Map.Entry entry = (Map.Entry) iter.next();
                String key = (String) entry.getKey();

                // the first attribute is the class (-1 or 1)
                if (i == 0) {
                    labels = new FastVector();
                    labels.addElement("-1");
                    labels.addElement("1");
                    atts.addElement(new Attribute(key, labels));
                    String label = entry.getValue().toString();
                    Double idx = new Double(label);
                    values.add(idx);
                } else {
                    // - otherwise we are assuming they are all numeric
                    atts.addElement(new Attribute(key));
                    Double feature = Double.parseDouble(entry.getValue().toString());
                    values.add(feature);
                }
            }

            // 2. create Instances object
            System.out.println("i is "+i);
            data = new Instances("CLIClassifier", atts, 1);

            Double[] Doubles = (Double[]) values.toArray(new Double[i]);
            double[] doubles = ArrayUtils.toPrimitive(Doubles);

            data.add(new Instance(1.0, doubles));
            System.out.println("Got:");
            System.out.println(data);

        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
