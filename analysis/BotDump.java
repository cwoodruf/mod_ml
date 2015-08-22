/**
 * make a weka arff format file based on a random sample 
 * from the botstats table
 * some mod_ml preprocessor (defined by the MLPreprocessor directive)
 * is needed to fill the botstats table with useful information
 * see botlogger.py and botlog.py
 * 
 * the output of this program can be used to build serialized
 * weka models that can be used with BotClassifier to 
 * make predictions based on saved stats for a host/ip pair
 * 
 * references:
 * see https://weka.wikispaces.com/Creating+an+ARFF+file
 */
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;
import java.sql.PreparedStatement;
import java.sql.SQLException;

import java.io.PrintWriter;

import java.util.Map;
import java.util.HashMap;
import java.util.regex.Pattern;
import java.util.regex.Matcher;
import java.util.Random;

import weka.core.Attribute;
import weka.core.FastVector;
import weka.core.Instance;
import weka.core.Instances;

/**
 * dump the contents of a stats table like botstats 
 * in arff format
 *
 * @author cal
 */
public class BotDump {

    public static boolean verbose = false;
    public static String usage = 
        "Usage: java [java args] BotDump {table} {file} [percent: default=60] [sample: id (0=epoch)] [boost? 1|0]";

    // database stuff
    private static String dbhost = null;
    private static String dbport = null;
    private static String dbname = null;
    private static String dbuser = null;
    private static String dbpw = null;
    private static String dsn = null;
    private static Connection conn = null;

    // size of results from our query - must match what the models expect
    private static int BOTSTATSZ = 16;
    // needed for making attributes
    private static String[] keys = new String[BOTSTATSZ];

    // table to find data on a particular IP
    // this table must be equivalent to the source from which the models were generated
    private static String stats = "{stats table}"; 
    private static int pct = 60;
    private static long sample = 0; // used to identify what was used to build model
    private static String file = null;
    private static PrintWriter dumper = null;
    private static String query = null;
    private static boolean boost = true; // bump up # number of examples when too few

    /**
     * make an arbitrarily large set of attributes
     * where the first one is a 1,-1 label
     * and the rest are numbers
     */
    public static FastVector mkAtts() {
        FastVector atts = new FastVector();
        if (keys[0] == null) setKeys();

        for (int i=0; i<keys.length; i++) {
            // the first attribute is the class (-1 or 1)
            if (i == 0) {
                FastVector labels = new FastVector();
                labels.addElement("-1");
                labels.addElement("1");
                atts.addElement(new Attribute(keys[i], labels));
            } else {
                // - otherwise we are assuming they are all numeric
                atts.addElement(new Attribute(keys[i]));
            }
        }
        return atts;
    }

    /**
     * consistent with other applications using the mod_ml db we're using 
     * the environment variables too
     */
    public static void dbSetup() {
        // see https://docs.oracle.com/javase/tutorial/essential/environment/env.html
        Map<String, String> env = System.getenv();
        dbuser = env.get("ML_DB_USER");
        dbpw = env.get("ML_DB_PW");
        dbname = env.get("ML_DB");
        dbhost = env.get("ML_DB_HOST");
        if (dbhost == null) dbhost = "localhost";
        dbport = env.get("ML_DB_PORT");
        if (dbport == null) dbport = "5432";
        dsn = "jdbc:postgresql://"+dbhost+":"+dbport+"/"+dbname;
        if (verbose) System.err.println("dsn: "+dsn);
    }

    /**
     * connect to postgres db using envvars with the relevant parameters
     */
    public static Connection dbConnect() {
        if (dsn == null) dbSetup();
        Connection conn = null;
        try {
            // see http://www.tutorialspoint.com/postgresql/postgresql_java.htm
            Class.forName("org.postgresql.Driver");
            conn = DriverManager.getConnection(dsn,dbuser,dbpw);
        } catch (Exception e) {
            System.err.println("connect failed for dsn "+dsn);
            System.err.println(
                    "do you have envvars ML_DB_HOST, ML_DB_PORT, "+
                    "ML_DB, ML_DB_USER and ML_DB_PW set?");
            System.exit(1);
        }
        return conn;
    }

    /**
     * dump our stats records and make instances out of them
     */
    public static void botstats() {
        FastVector atts = null;
        PreparedStatement st = null;
        ResultSet rs = null;
        Instances data = null;
        try {
            atts = mkAtts();
            data = new Instances("botstats", atts, 1);

            conn = dbConnect();
            // from http://docs.oracle.com/javase/tutorial/jdbc/basics/prepared.html
            Statement pickst = conn.createStatement();
            if (sample == 0) {
                sample = (long) (System.currentTimeMillis()/1000L);
            }
            System.err.println("using sample="+sample+" to identify rows used for model");
            // figure out what limit should be for the different classes based on a given pct
            // if we have a very unbalanced ratio of postitive to negative examples
            // figure out how many more examples we need to keep to keep things even
            Statement countst = conn.createStatement();
            ResultSet countres = countst.executeQuery(
                    "select class,count(*) as num from "+stats+
                    " where mean is not null and class in (-1,1) "+
                    " group by class");
            HashMap<Integer,Integer> multipliers = new HashMap<Integer,Integer>();
            HashMap<Integer,Long> limits = new HashMap<Integer,Long>();
            int max = 0;
            long total = 0;
            long limit = 0;
            while (countres.next()) {
                int cl = countres.getInt("class");
                int count = countres.getInt("num");
                multipliers.put(cl,count);
                long thislimit = (long) (count * pct/100.0);
                limits.put(cl,thislimit);
                limit += thislimit;
                pickst.executeUpdate(
                        "update botstats "+
                        " set sample="+sample+" "+
                        " where class is not null and ip in "+
                        " (select ip from botstats "+
                          " where mean is not null and class is not null and sample is null "+
                          " and class='"+cl+"' "+
                          " order by random() limit "+thislimit+")");
                total += count;
                if (count > max) max = count;
            }
            for (Map.Entry<Integer,Integer> entry: multipliers.entrySet()) {
                int cl = entry.getKey();
                int count = entry.getValue();
                if (boost) multipliers.put(cl, (max/count));
                else multipliers.put(cl, 1);
            }
            System.err.println("selected "+limit+" rows");
            query = mkQuery(stats,sample);
            st = conn.prepareStatement(query);
            // st.setString(1, ip);
            if (verbose) System.err.println(st);
            rs = st.executeQuery();

            data.setClassIndex(0);
            long instances = 0;
            while (rs.next()) {
                // for this application we don't really care what the actual designation is
                double[] row = new double[BOTSTATSZ];
                int cl = rs.getInt("class");
                int i = 0;
                row[i++] = (cl == 1 ? 1 : 0); 
                row[i++] = rs.getDouble("mean"); 
                row[i++] = rs.getDouble("var"); 
                row[i++] = rs.getDouble("skew"); 
                row[i++] = rs.getDouble("kurtosis"); 
                row[i++] = rs.getDouble("hmean"); 
                row[i++] = rs.getDouble("hvar"); 
                row[i++] = rs.getDouble("hskew"); 
                row[i++] = rs.getDouble("hkurtosis"); 
                row[i++] = rs.getDouble("htmean"); 
                row[i++] = rs.getDouble("htvar"); 
                row[i++] = rs.getDouble("htskew"); 
                row[i++] = rs.getDouble("htkurtosis"); 
                row[i++] = rs.getDouble("poverr"); 
                row[i++] = rs.getDouble("uacount"); 
                row[i++] = rs.getDouble("errprop"); 

                if (verbose) {
                    for (i = 0; i<row.length; i++) {
                        System.err.println(row[i]);
                    }
                }

                int amplify = multipliers.get(cl);
                for (i = 0; i < amplify; i++) {
                    ++instances;
                    data.add(new Instance(1.0, row));
                }
            }
            System.err.println("actually saved "+instances+" instances");
            data.randomize(new Random());
            if (dumper == null) {
                System.out.println(data);
            } else {
                dumper.println(data);
                dumper.flush();
                dumper.close();
            }
            System.err.println("success!");
            System.exit(0);

        } catch (Exception e) {
            // also from the tutorialspoint pg demo
            System.err.println(e.getClass().getName()+": "+e.getMessage());
            e.printStackTrace();

        } finally {
            // from http://www.mkyong.com/jdbc/jdbc-preparestatement-example-select-list-of-the-records/
            try {
                if (rs != null) { rs.close(); }
                if (st != null) { st.close(); }
            } catch (SQLException e) {
                // do nothing
            }
		}
    }

    /**
     * make some stuff for integrating database data with weka
     */
    public static void setKeys() {
        // this array is needed for making attributes for classification
        int i = 0;
        keys[i++] = "class";
        keys[i++] = "mean";
        keys[i++] = "var";
        keys[i++] = "skew";
        keys[i++] = "kurtosis";
        keys[i++] = "hmean";
        keys[i++] = "hvar";
        keys[i++] = "hskew";
        keys[i++] = "hkurtosis";
        keys[i++] = "htmean";
        keys[i++] = "htvar";
        keys[i++] = "htskew";
        keys[i++] = "htkurtosis";
        keys[i++] = "poverr";
        keys[i++] = "uacount";
        keys[i++] = "errprop";
    }
    /**
     * query needed to get data from stats table
     * note that some fields are compound
     */
    public static String mkQuery(String table,long sample) {
        String query = 
            "select "+
                "class,"+
                "mean,"+
                "var,"+
                "skew,"+
                "kurtosis,"+
                "hmean,"+
                "hvar,"+
                "hskew,"+
                "hkurtosis,"+
                "htmean,"+
                "htvar,"+
                "htskew,"+
                "htkurtosis,"+
                "pages/reqs as poverr,"+
                "array_length(uas,1) as uacount,"+
                "errs/reqs as errprop "+
                " from "+table+" where sample="+sample;
        return query;
    }

    /**
     * read the http_host and remote_addr from the command line
     * look up relevant stats data for my models
     * make weka compatible instance data for this if it exists
     * make a prediction
     * return the result
     */
    public static void main(String[] args) throws Exception {
        usage = usage + "\nthe models and stats table need to use the same number and type of fields";
        usage = usage + "\nthis query must work:\n"+mkQuery(stats,sample);

        if (args.length < 2) {
            System.err.println(usage);
            System.exit(1);
        }

        stats = args[0];
        Pattern istable = Pattern.compile("^[a-zA-Z]\\w+$");
        if (!istable.matcher(stats).find()) {
            System.err.println("invalid table name: "+stats);
            System.err.println(usage);
            System.exit(1);
        }
        if (args.length >= 2) {
            file = args[1];
            try {
                dumper = new PrintWriter(file,"UTF-8");
                System.err.println("Saving output to "+file);
            } catch (Exception e) {
                System.err.println("Error opening file "+file+" for output "+e.getMessage());
                System.exit(1);
            }
        }
        if (args.length >= 3) {
            pct = Integer.parseInt(args[2]);
            System.err.println("pct now "+pct);
        }
        if (args.length >= 4) {
            sample = Long.parseLong(args[3]);
            System.err.println("sample id now "+sample);
        }
        if (args.length >= 5) {
            boost = ((Integer.parseInt(args[4]) > 0) ? true : false);
            if (boost == true) {
                System.err.println("boosting sparse examples");
            } else {
                System.err.println("turning off example boost");
            }
        }
        botstats();
    }
}

