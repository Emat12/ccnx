/*
 * A CCNx library test.
 *
 * Copyright (C) 2010, 2011 Palo Alto Research Center, Inc.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation. 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details. You should have received
 * a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * PD org.ccnx.ccn.test
 */
package org.ccnx.ccn.test;

import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.protocol.CCNTime;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.MalformedContentNameStringException;

/**
 * Utility class to provide facilities to be used by all of
 * the CCN tests, most importantly a standardized namespace for
 * them to write their data into.
 * 
 * Given a unit test class named UnitTestClass, we name all the data generated
 * by that test class as 
 *  /ccnx.org/test/UnitTestClass-TIMESTAMP
 *  
 * for a unit test UnitTest in that class, ideally the data for that specific unit test
 * will be stored under 
 *  /ccnx.org/test/UnitTestClass-TIMESTAMP/UnitTest
 *  
 * To use, make a static CCNTestHelper in your test class, e.g.:
 * 
 * static CCNTestHelper testHelper = new CCNTestHelper(TestClass.class);
 * 
 * and then in a test called TestFoo, get the name prefix to use for generated data by:
 * 
 * ContentName dataPrefix = testHelper.getTestNamespace("TestFoo");
 * 
 * for data shared across tests, use the class-level name prefix:
 * 
 * ContentName sharedFileName = ContentName.fromNative(testHelper.getClassNamespace(), "shared_file.txt");
 * 
 */
public class CCNTestHelper {

	protected static final String TEST_PREFIX_STRING = "/ccnx.org/test";
	protected static ContentName TEST_PREFIX;
	
	ContentName _testNamespace;
	String _testName;
	CCNTime _testTime;
	
	static {
		try {
			TEST_PREFIX = ContentName.fromNative(TEST_PREFIX_STRING);
		} catch (MalformedContentNameStringException e) {
			Log.warning(Log.FAC_TEST, "Cannot parse default test namespace name {1}!", TEST_PREFIX_STRING);
			throw new RuntimeException("Cannot parse default test namespace name: " + TEST_PREFIX_STRING +".");
		}
	}
	
	protected ContentName testPrefix() { return TEST_PREFIX; }
	
	/**
	 * Create a test helper for a named unit test class.
	 * @param testClassName The class name, if the package is included it will be stripped.
	 */
	public CCNTestHelper(String testClassName) {
		this(null, testClassName);
	}
	
	public CCNTestHelper(ContentName externalPrefix, String testClassName) {
		
		if (null == externalPrefix) {
			externalPrefix = testPrefix();
		}
		
		_testName = testClassName;
		if (_testName.contains(".")) {
			_testName = testClassName.substring(testClassName.lastIndexOf(".") + 1);
		}
		_testTime = new CCNTime();
		_testNamespace = ContentName.fromNative(externalPrefix, _testName + "-" + _testTime.toShortString());
		Log.info(Log.FAC_TEST, "Initializing test {0}, data can be found under {1}.", testClassName, _testNamespace);		
	}
	
	/**
	 * Create a test helper for a specified unit test class.
	 * @param unitTestClass The class containing the unit tests.
	 */
	public CCNTestHelper(Class<?> unitTestClass) {
		this(unitTestClass.getName().toString());
	}
	
	/**
	 * Create a test helper for a specified unit test class.
	 * @param unitTestClass The class containing the unit tests.
	 */
	public CCNTestHelper(ContentName externalPrefix, Class<?> unitTestClass) {
		this(externalPrefix, unitTestClass.getName().toString());
	}

	/**
	 * Retrieve the timestamped, top-level name for data generated by this set of test cases.
	 * @return the name prefix to use for data shared across this set of unit tests.
	 */
	public ContentName getClassNamespace() { return _testNamespace; }
	
	/**
	 * Helper method to build child names for the class under test.
	 */
	public ContentName getClassChildName(String childName) {
		return ContentName.fromNative(_testNamespace, childName);
	}

	/**
	 * Retrieve a name for data for a specific test.
	 * @param testName the name of the test method, as a string
	 * @return the name prefix to use for data generated by that test
	 */
	public ContentName getTestNamespace(String testName) { 
		return ContentName.fromNative(_testNamespace, testName); }
	
	/**
	 * Helper method to build child names for a specific test.
	 */
	public ContentName getTestChildName(String testName, String childName) {
		return ContentName.fromNative(_testNamespace, new String[]{testName, childName});
	}
	
	/**
	 * Get the timestamp used for this test run. All data is gathered under the same timestamp.
	 * @return the timestamp
	 */
	public CCNTime getTestTime() { return _testTime; }
}
